#include "PythonNetwork.h"

#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include <numpy/arrayobject.h>

#include "Config.h"

thread_local PyGILState_STATE PythonContext::GilState;
thread_local PyThreadState* PythonContext::ThreadState = nullptr;

PythonContext::PythonContext()
{
    // Re-acquire the GIL.
    if (!ThreadState)
    {
        GilState = PyGILState_Ensure();
    }
    else
    {
        PyEval_RestoreThread(ThreadState);
    }

    // Ensure numpy is initialized.
    if (!PyArray_API)
    {
        _import_array();
    }
}

PythonContext::~PythonContext()
{
    // Release the GIL.
    ThreadState = PyEval_SaveThread();
}

RawPrediction::RawPrediction(float value, OutputPlanesPtr policy)
    : _value(value)
{
    constexpr int count = sizeof(_policy) / sizeof(float);
    std::copy(reinterpret_cast<float*>(policy), reinterpret_cast<float*>(policy) + count, reinterpret_cast<float*>(_policy.data()));
}

float RawPrediction::Value() const
{
    return _value;
}

void* RawPrediction::Policy()
{
    return _policy.data();
}

BatchedPythonNetwork::BatchedPythonNetwork()
    : _enabled(true)
{
    PythonContext context;

    _module = PyImport_ImportModule("network");
    assert(_module);

    _predictBatchFunction = PyObject_GetAttrString(_module, "predict_batch");
    assert(_predictBatchFunction);
    assert(PyCallable_Check(_predictBatchFunction));

    _trainBatchFunction = PyObject_GetAttrString(_module, "train_batch");
    assert(_trainBatchFunction);
    assert(PyCallable_Check(_trainBatchFunction));

    _saveNetworkFunction = PyObject_GetAttrString(_module, "save_network");
    assert(_saveNetworkFunction);
    assert(PyCallable_Check(_saveNetworkFunction));
}

BatchedPythonNetwork::~BatchedPythonNetwork()
{
    Py_XDECREF(_saveNetworkFunction);
    Py_XDECREF(_trainBatchFunction);
    Py_XDECREF(_predictBatchFunction);
    Py_XDECREF(_module);
}

void BatchedPythonNetwork::SetEnabled(bool enabled)
{
    std::unique_lock lock(_mutex);

    if (_enabled == enabled)
    {
        return;
    }

    _enabled = enabled;

    // If the network is currently disabled then this should be a no-op, so no need to check enabled vs. disabled.
    for (auto pair : _predictQueue)
    {
        pair.second->Push(nullptr);
    }
    _predictQueue.clear();
}

IPrediction* BatchedPythonNetwork::Predict(InputPlanes& image)
{
    SyncQueue<IPrediction*> output;
    
    // Safely push to the worker queue.
    {
        std::unique_lock lock(_mutex);

        // Return "nullptr" to indicate that the network is disabled and caller should stop working.
        // They may also get this "nullptr" via Pop() if the network is disabled with a partial batch.
        if (!_enabled)
        {
            return nullptr;
        }

        _predictQueue.emplace_back(&image, &output);

        // Only send a wake-up on the (BatchSize-1) to BatchSize transition, to avoid spam.
        if (_predictQueue.size() == BatchSize)
        {
            _condition.notify_one();
        }
    }

    // Wait for the worker thread to process the batch.
    return output.Pop();
}

void BatchedPythonNetwork::TrainBatch(int step, InputPlanes* images, float* values, OutputPlanes* policies)
{
    PythonContext context;

    PyObject* pythonStep = PyLong_FromLong(step);
    assert(pythonStep);

    npy_intp imageDims[4]{ Config::BatchSize, 12, 8, 8 };
    PyObject* pythonImages = PyArray_SimpleNewFromData(
        Py_ARRAY_LENGTH(imageDims), imageDims, NPY_FLOAT32, images);
    assert(pythonImages);

    npy_intp valueDims[1]{ Config::BatchSize };
    PyObject* pythonValues = PyArray_SimpleNewFromData(
        Py_ARRAY_LENGTH(valueDims), valueDims, NPY_FLOAT32, values);
    assert(pythonValues);

    npy_intp policyDims[4]{ Config::BatchSize, 73, 8, 8 };
    PyObject* pythonPolicies = PyArray_SimpleNewFromData(
        Py_ARRAY_LENGTH(policyDims), policyDims, NPY_FLOAT32, policies);
    assert(pythonPolicies);

    PyObject* tupleResult = PyObject_CallFunctionObjArgs(_trainBatchFunction, pythonStep, pythonImages, pythonValues, pythonPolicies, nullptr);
    assert(tupleResult);

    Py_XDECREF(tupleResult);
    Py_DECREF(pythonPolicies);
    Py_DECREF(pythonValues);
    Py_DECREF(pythonImages);
    Py_DECREF(pythonStep);
}

void BatchedPythonNetwork::SaveNetwork(int checkpoint)
{
    PythonContext context;

    PyObject* pythonCheckpoint = PyLong_FromLong(checkpoint);
    assert(pythonCheckpoint);

    PyObject* tupleResult = PyObject_CallFunctionObjArgs(_saveNetworkFunction, pythonCheckpoint, nullptr);
    assert(tupleResult);

    Py_DECREF(tupleResult);
    Py_DECREF(pythonCheckpoint);
}

void BatchedPythonNetwork::Work()
{
    while (true)
    {
        // Wait until the queue is full enough to process.
        std::unique_lock lock(_mutex);
        _condition.wait(lock, [&] { return (_predictQueue.size() >= BatchSize); });

        // Drain and unlock as quickly as possible.
        std::vector<std::pair<InputPlanes*, SyncQueue<IPrediction*>*>> batch(BatchSize);
        for (int i = 0; i < BatchSize; i++)
        {
            batch[i] = _predictQueue.front();
            _predictQueue.pop_front();
        }
        lock.unlock();

        // Combine images.
        npy_intp dims[4]{ BatchSize, 12, 8, 8 };
        std::unique_ptr<std::array<std::array<std::array<std::array<float, 8>, 8>, 12>, BatchSize>> batchImage(
            new std::array<std::array<std::array<std::array<float, 8>, 8>, 12>, BatchSize>());
        for (int i = 0; i < BatchSize; i++)
        {
            (*batchImage)[i] = *batch[i].first;
        }

        {
            PythonContext context;

            // Make the predict call.
            PyObject* pythonBatchImage = PyArray_SimpleNewFromData(
                Py_ARRAY_LENGTH(dims), dims, NPY_FLOAT32, batchImage->data());
            assert(pythonBatchImage);

            PyObject* tupleResult = PyObject_CallFunctionObjArgs(_predictBatchFunction, pythonBatchImage, nullptr);
            assert(tupleResult);

            // Extract the values.
            PyObject* pythonValue = PyTuple_GetItem(tupleResult, 0); // PyTuple_GetItem does not INCREF
            assert(PyArray_Check(pythonValue));

            PyArrayObject* pythonValueArray = reinterpret_cast<PyArrayObject*>(pythonValue);
            float* values = reinterpret_cast<float*>(PyArray_DATA(pythonValueArray));

            // Extract the policies.
            PyObject* pythonPolicy = PyTuple_GetItem(tupleResult, 1); // PyTuple_GetItem does not INCREF
            assert(PyArray_Check(pythonPolicy));

            PyArrayObject* pythonPolicyArray = reinterpret_cast<PyArrayObject*>(pythonPolicy);
            float(*policies)[73][8][8] = reinterpret_cast<float(*)[73][8][8]>(PyArray_DATA(pythonPolicyArray));

            // Deliver predictions.
            for (int i = 0; i < BatchSize; i++)
            {
                batch[i].second->Push(new RawPrediction(values[i], policies[i]));
            }

            Py_DECREF(tupleResult);
            Py_DECREF(pythonBatchImage);
        }
    }
}