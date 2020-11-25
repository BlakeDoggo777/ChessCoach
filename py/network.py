# --- Require silent TensorFlow initialization when running as UCI ---

import os
import socket

silent = bool(os.environ.get("CHESSCOACH_SILENT"))
if silent:
  os.environ["TF_CPP_MIN_LOG_LEVEL"] = "3"

def log(*args):
  if not silent:
    print(*args)

import tensorflow as tf

# --- TPU/GPU initialization ---

tpu_strategy = None
try:
  # Passing zone-qualified cluster-injected TPU_NAME to the resolver fails, need to leave it blank.
  tpu_name = os.environ.get("TPU_NAME") or socket.gethostname()
  if "/" in tpu_name:
    tpu_name = None
  log("TPU name for cluster resolution:", tpu_name, "" if tpu_name else "(auto)")
  resolver = tf.distribute.cluster_resolver.TPUClusterResolver(tpu=tpu_name)
  tf.config.experimental_connect_to_cluster(resolver)
  tf.tpu.experimental.initialize_tpu_system(resolver)
  tpu_strategy = tf.distribute.TPUStrategy(resolver)
  log("Found TPU")
except:
  log("No TPU or error resolving")

tpus = tf.config.experimental.list_logical_devices("TPU")
gpus = tf.config.experimental.list_logical_devices("GPU")
devices = tpus + gpus
thread_ident_to_index = {}
log(f"TPU devices: {[t.name for t in tpus]}")
log(f"GPU devices: {[g.name for g in gpus]}")

# --- Now it's safe for further imports ---

import math
import re
import time
import threading
import numpy as np

from config import Config, PredictionStatus
from model import ModelBuilder
import transformer
from training import Trainer
from dataset import DatasetBuilder

# --- Network ---

class PredictionModels:
  def __init__(self):
    self.full = None
    self.full_weights_path = None
    self.full_weights_last_check = None
    self.predict = None

class TrainingModels:
  def __init__(self):
    self.full = None
    self.train = None
    self.commentary_encoder = None
    self.commentary_decoder = None
    self.commentary_tokenizer = None

class Network:

  def __init__(self, config, network_type, model_builder, name):
    self.config = config
    self.network_type = network_type
    self.model_builder = model_builder
    self.training_compiler = None
    self._name = name
    self.initialize()

  @property
  def name(self):
    return self._name

  @name.setter
  def name(self, value):
    self._name = value

    # Clear out any loaded models, ready to lazy-load using the new name.
    self.initialize()

  def initialize(self):
    self.models_predict = [PredictionModels() for _ in devices]
    self.models_train = TrainingModels()
    self.tensorboard_writer_training = None
    self.tensorboard_writer_validation = None

  @property
  def info(self):
    path = self.latest_network_path()
    step_count = int(re.match(".*?([0-9]+)$", path).group(1)) if path else 0
    training_chunk_count = config.count_training_chunks()
    return (step_count, training_chunk_count)

  @tf.function
  def tf_predict(self, device_index, images):
      return self.models_predict[device_index].predict(images, training=False)

  @tf.function
  def tf_predict_for_training(self, images):
      return self.models_train.train(images, training=False)

  def predict_batch(self, device_index, images):
    status = self.ensure_prediction(device_index)
    return (status, *self.tf_predict(device_index, images))

  def predict_commentary_batch(self, images):
    networks.teacher.ensure_commentary()

    encoder = self.models_train.commentary_encoder
    decoder = self.models_train.commentary_decoder
    tokenizer = self.models_train.commentary_tokenizer

    start_token = tokenizer.word_index[ModelBuilder.token_start]
    end_token = tokenizer.word_index[ModelBuilder.token_end]
    max_length = ModelBuilder.transformer_max_length

    sequences = transformer.predict_greedy(encoder, decoder,
      start_token, end_token, max_length, images)

    def trim_start_end_tokens(sequence):
      for i, token in enumerate(sequence):
        if (token == end_token):
          return sequence[1:i]
      return sequence[1:]

    sequences = [trim_start_end_tokens(s) for s in np.array(memoryview(sequences))]
    comments = tokenizer.sequences_to_texts(sequences)
    comments = np.array([c.encode("utf-8") for c in comments])
    return comments

  def ensure_full(self, device_index):
    with ensure_locks[device_index]:
      # The full model may already exist.
      models = self.models_predict[device_index]
      if models.full:
        return

      models.full, models.full_weights_path = self.build_full(device_index)
      models.full_weights_last_check = time.time()
  
  def build_full(self, log_device_context):
    # Either load it from disk, or create a new one.
    with model_creation_lock:
      network_path = self.latest_network_path()
      if network_path:
        log_name = self.get_log_name(network_path)
        log(f"Loading model ({log_device_context}/{self.network_type}/full): {log_name}")
        model_full_path = self.model_full_path(network_path)
        full = self.model_builder()
        self.load_weights(full, model_full_path)
      else:
        log(f"Creating new model ({log_device_context}/{self.network_type}/full)")
        full = self.model_builder()
        model_full_path = None
      return full, model_full_path

  def maybe_check_update_full(self, device_index):
    interval_seconds = self.config.self_play["network_update_check_interval_seconds"]
    models = self.models_predict[device_index]
    now = time.time()
    if (now - models.full_weights_last_check) > interval_seconds:
      models.full_weights_last_check = now
      return self.check_update_full(device_index)
    return PredictionStatus.Nothing

  def check_update_full(self, device_index):
    models = self.models_predict[device_index]
    network_path = self.latest_network_path()
    if network_path:
      # Weights paths for the same network name and type will be identical up until
      # the 9-digit zero-padded step number, which we can compare lexicographically
      # with greater meaning more recent, and coalescing the empty string for no weights
      # (i.e. created from scratch).
      model_full_path = self.model_full_path(network_path)
      newer_weights_available = ((models.full_weights_path or "") < model_full_path)
      if newer_weights_available:
        log_name = self.get_log_name(network_path)
        log(f"Updating model ({device_index}/{self.network_type}/full): {log_name}")
        self.load_weights(models.full, model_full_path)
        models.full_weights_path = model_full_path
        return PredictionStatus.UpdatedNetwork
    return PredictionStatus.Nothing

  def ensure_prediction(self, device_index):
    with ensure_locks[device_index]:
      # The prediction model may already exist.
      if self.models_predict[device_index].predict:
        # Occasionally check for more recent weights to load.
        return self.maybe_check_update_full(device_index)

      # Take the prediction subset from the full model.
      self.ensure_full(device_index)
      self.models_predict[device_index].predict = ModelBuilder().subset_predict(self.models_predict[device_index].full)
      return PredictionStatus.Nothing
  
  def ensure_training(self):
    # The training subset may already exist.
    if self.models_train.train:
      return self.models_train.train

    # Build a full model.
    self.models_train.full, _ = self.build_full("training")

    # Take the training subset from the full model.
    self.models_train.train = ModelBuilder().subset_train(self.models_train.full)

    # Compile the new subset for training.
    self.training_compiler(self.models_train.train)

    # Set up TensorBoard.
    tensorboard_network_path = self.config.join(self.config.misc["paths"]["tensorboard"], self._name, self.network_type)
    self.tensorboard_writer_training = tf.summary.create_file_writer(self.config.join(tensorboard_network_path, "training"))
    self.tensorboard_writer_validation = tf.summary.create_file_writer(self.config.join(tensorboard_network_path, "validation"))

    return self.models_train.train

  def ensure_commentary(self):
    # The encoder, decoder and tokenizer may already exist.
    if self.models_train.commentary_encoder:
      return

    # Take the encoder subset from the full training model.
    self.ensure_training()
    self.models_train.commentary_encoder = ModelBuilder().subset_commentary_encoder(self.models_train.full)

    # Either load decoder and tokenizer from disk, or create new.
    with model_creation_lock:
      log_device_context = "training"
      network_path = self.latest_network_path()
      if network_path:
        log_name = self.get_log_name(network_path)
        log(f"Loading model ({log_device_context}/{self.network_type}/commentary): {log_name}")

        # Load the decoder.
        self.models_train.commentary_decoder = ModelBuilder().build_commentary_decoder(self.config)
        model_commentary_decoder_path = self.model_commentary_decoder_path(network_path)
        self.load_weights(self.models_train.commentary_decoder, model_commentary_decoder_path)

        # Load the tokenizer.
        commentary_tokenizer_path = self.commentary_tokenizer_path(network_path)
        tokenizer_json = tf.io.gfile.GFile(commentary_tokenizer_path, 'r').read()
        self.models_train.commentary_tokenizer = tf.keras.preprocessing.text.tokenizer_from_json(tokenizer_json)
      else:
        log(f"Creating new model ({log_device_context}/{self.network_type}/commentary)")
        self.models_train.commentary_decoder = ModelBuilder().build_commentary_decoder(self.config)
        self.models_train.commentary_tokenizer = ModelBuilder().build_tokenizer(self.config)

  # Storage may be slow, e.g. Google Cloud Storage, so retry.
  # For now, sleep for 1 second, up to 10 retry attempts (11 total).
  def load_weights(self, model, path):
    try:
      model.load_weights(path)
      return
    except:
      for _ in range(10):
        time.sleep(1.0)
        try:
          model.load_weights(path)
          return
        except:
          pass
    raise Exception(f"Failed to load weights from: {path}")

  def save(self, step):
    network_path = self.make_network_path(step)
    log_name = self.get_log_name(network_path)
    log_device_context = "training"

    # Save the full model from training.
    log(f"Saving model ({log_device_context}/{self.network_type}/full): {log_name}")
    model_full_path = self.model_full_path(network_path)
    self.models_train.full.save_weights(model_full_path, save_format="tf")

    # Save the commentary decoder and tokenizer if they exist.
    if self.models_train.commentary_decoder and self.models_train.commentary_tokenizer:
      log(f"Saving model ({log_device_context}/{self.network_type}/commentary): {log_name}")

      # Save the commentary decoder.
      model_commentary_decoder_path = self.model_commentary_decoder_path(network_path)
      self.models_train.commentary_decoder.save_weights(model_commentary_decoder_path, save_format="tf")

      # Save the tokenizer.
      commentary_tokenizer_path = self.commentary_tokenizer_path(network_path)
      tokenizer_json = self.models_train.commentary_tokenizer.to_json()
      tf.io.gfile.GFile(commentary_tokenizer_path, "w").write(tokenizer_json)

  def get_log_name(self, network_path):
    return os.path.basename(os.path.normpath(network_path))

  def latest_network_path(self):
    return self.config.latest_network_path_for_type(self.name, self.network_type)

  def make_network_path(self, step):
    parent_path = self.config.misc["paths"]["networks"]
    directory_name = f"{self.name}_{str(step).zfill(9)}"
    return self.config.join(parent_path, directory_name)

  def model_full_path(self, network_path):
    return self.config.join(network_path, self.network_type, "model", "weights")

  def model_commentary_decoder_path(self, network_path):
    return self.config.join(network_path, self.network_type, "commentary_decoder", "weights")

  def commentary_tokenizer_path(self, network_path):
    return self.config.join(network_path, self.network_type, "commentary_tokenizer.json")

# --- Networks ---

class Networks:

  def __init__(self, config, name="network"):
    self.config = config

    # Set by C++ via load_network depending on use-case.
    self._name = name

    # The teacher network uses the full 19*256 model.
    self.teacher = Network(config, "teacher", lambda: ModelBuilder().build(config), self._name)

    # The student network uses the smaller 8*64 model.
    self.student = Network(config, "student", lambda: ModelBuilder().build_student(config), self._name)

  @property
  def name(self):
    return self._name

  @name.setter
  def name(self, value):
    self._name = value
    self.teacher.name = self._name
    self.student.name = self._name

  def log(self, *args):
    log(*args)

# --- Helpers ---

thread_local = threading.local()

def choose_device_index():
  try:
    return thread_local.device_index
  except:
    pass
  thread_ident = threading.get_ident()
  with device_lock:
    next_index = len(thread_ident_to_index)
    thread_index = thread_ident_to_index.setdefault(thread_ident, next_index)
  device_index = thread_index % len(devices)
  thread_local.device_index = device_index
  return device_index

def device(device_index):
  return tf.device(devices[device_index].name)

# --- C++ API ---

def predict_batch_teacher(images):
  device_index = choose_device_index()
  with device(device_index):
    status, value, policy = networks.teacher.predict_batch(device_index, images)
    return status, np.array(memoryview(value)), np.array(memoryview(policy))

def predict_batch_student(images):
  device_index = choose_device_index()
  with device(device_index):
    status, value, policy = networks.student.predict_batch(device_index, images)
    return status, np.array(memoryview(value)), np.array(memoryview(policy))

def predict_commentary_batch(images):
  # Always use the teacher network for commentary.
  return networks.teacher.predict_commentary_batch(images)

def train_teacher(game_types, training_windows, step, checkpoint):
  trainer.train(networks.teacher, None, game_types, training_windows, step, checkpoint)

def train_student(game_types, training_windows, step, checkpoint):
  trainer.train(networks.student, networks.teacher, game_types, training_windows, step, checkpoint)

def train_commentary(step, checkpoint):
  # Always use the teacher network for commentary.
  trainer.train_commentary(networks.teacher, step, checkpoint)

def log_scalars_teacher(step, names, values):
  trainer.log_scalars(networks.teacher, step, names, values)

def log_scalars_student(step, names, values):
  trainer.log_scalars(networks.student, step, names, values)

def load_network(network_name):
  log("Network:", network_name)
  networks.name = network_name

def get_network_info_teacher():
  return networks.teacher.info

def get_network_info_student():
  return networks.student.info

def save_network_teacher(checkpoint):
  networks.teacher.save(checkpoint)
  
def save_network_student(checkpoint):
  networks.student.save(checkpoint)

def save_file(relative_path, data):
  config.save_file(relative_path, data)

def debug_decompress(result, image_pieces_auxiliary, mcts_values, policy_row_lengths, policy_indices, policy_values):
  images, values, policies, reply_policies = datasets.decompress(result, image_pieces_auxiliary,
    mcts_values, policy_row_lengths, policy_indices, policy_values)
  return np.array(memoryview(images)), np.array(memoryview(values)), np.array(memoryview(policies)), np.array(memoryview(reply_policies))

# --- Initialize ---

# Build the device mapping safely.
device_lock = threading.Lock()
# Prevent fine-grained races in get-or-create logic.
ensure_locks = [threading.RLock() for _ in devices]
# Only create one TensorFlow/Keras model at a time, even if on different devices.
model_creation_lock = threading.Lock()

config = Config(bool(tpu_strategy))
networks = Networks(config)
datasets = DatasetBuilder(config)
trainer = Trainer(networks, tpu_strategy, devices, datasets)