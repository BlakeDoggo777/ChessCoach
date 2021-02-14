#ifndef _WORKERGROUP_H_
#define _WORKERGROUP_H_

#include <vector>
#include <thread>

#include "SelfPlay.h"

class WorkerGroup
{
public:

    bool IsInitialized();
    void ShutDown();

    template <typename Function>
    void Initialize(INetwork* network, NetworkType networkType, int workerCount, int workerParallelism, Function workerLoop)
    {
        workCoordinator.reset(new WorkCoordinator(workerCount));
        controllerWorker.reset(new SelfPlayWorker(nullptr /* storage */, &searchState, 1 /* gameCount */));
        for (int i = 0; i < workerCount; i++)
        {
            const bool primary = (i == 0);
            selfPlayWorkers.emplace_back(new SelfPlayWorker(nullptr /* storage */, &searchState, workerParallelism));
            selfPlayThreads.emplace_back(workerLoop, selfPlayWorkers[i].get(), workCoordinator.get(), network, networkType, primary);
        }
    }

    SearchState searchState{};
    std::unique_ptr<WorkCoordinator> workCoordinator;
    std::unique_ptr<SelfPlayWorker> controllerWorker;
    std::vector<std::unique_ptr<SelfPlayWorker>> selfPlayWorkers;
    std::vector<std::thread> selfPlayThreads;
};

#endif // _WORKERGROUP_H_