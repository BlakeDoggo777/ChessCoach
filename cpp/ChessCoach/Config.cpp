#include "Config.h"

#include <map>
#include <vector>
#include <functional>

#include <toml11/toml.hpp>

#include "Platform.h"

using TomlValue = toml::basic_value<toml::discard_comments, std::map, std::vector>;

const std::map<std::string, StageType> StageTypeLookup = {
    { "play", StageType_Play },
    { "train", StageType_Train },
    { "train_commentary", StageType_TrainCommentary },
    { "save", StageType_Save },
    { "strength_test", StageType_StrengthTest },
};

const std::map<std::string, NetworkType> NetworkTypeLookup = {
    { "teacher", NetworkType_Teacher },
    { "student", NetworkType_Student },
    { "", NetworkType_Count },
};

const std::map<std::string, GameType> GameTypeLookup = {
    { "supervised", GameType_Supervised },
    { "training", GameType_Training },
    { "validation", GameType_Validation },
    { "", GameType_Count },
};
static_assert(GameType_Count == 3);

std::vector<StageConfig> ParseStages(const TomlValue& config)
{
    std::vector<TomlValue> configStages = config.as_array();
    std::vector<StageConfig> stages(configStages.size());
    
    for (int i = 0; i < configStages.size(); i++)
    {
        // Required (find)
        stages[i].Stage = StageTypeLookup.at(toml::find<std::string>(configStages[i], "stage"));

        // Optional (find_or)
        stages[i].Target = NetworkTypeLookup.at(toml::find_or<std::string>(configStages[i], "target", ""));
        stages[i].Type = GameTypeLookup.at(toml::find_or<std::string>(configStages[i], "type", ""));
        stages[i].WindowSizeStart = toml::find_or<int>(configStages[i], "window_size_start", 0);
        stages[i].WindowSizeFinish = toml::find_or<int>(configStages[i], "window_size_finish", 0);
        stages[i].NumGames = toml::find_or<int>(configStages[i], "num_games", 0);
    }

    return stages;
}

struct DefaultPolicy
{
    template <typename T>
    T Find(const TomlValue& config, const toml::key& key, const T& defaultValue) const
    {
        return toml::find<T>(config, key);
    }
};

template <>
std::vector<StageConfig> DefaultPolicy::Find(const TomlValue& config, const toml::key& key, const std::vector<StageConfig>& defaultValue) const
{
    return ParseStages(config.at(key));
}

struct OverridePolicy
{
    template <typename T>
    T Find(const TomlValue& config, const toml::key& key, const T& defaultValue) const
    {
        return toml::find_or(config, key, defaultValue);
    }
};

template <>
std::vector<StageConfig> OverridePolicy::Find(const TomlValue& config, const toml::key& key, const std::vector<StageConfig>& defaultValue) const
{
    if (!config.is_table())
    {
        return defaultValue;
    }

    const auto& table = config.as_table();
    if (table.count(key) == 0)
    {
        return defaultValue;
    }

    return ParseStages(config.at(key));
}

template <typename Policy>
TrainingConfig ParseTraining(const TomlValue& config, const Policy& policy, const TrainingConfig& defaults)
{
    TrainingConfig training;

    training.BatchSize = policy.template Find<int>(config, "batch_size", defaults.BatchSize);
    training.CommentaryBatchSize = policy.template Find<int>(config, "commentary_batch_size", defaults.CommentaryBatchSize);
    training.Steps = policy.template Find<int>(config, "steps", defaults.Steps);
    training.PgnInterval = policy.template Find<int>(config, "pgn_interval", defaults.PgnInterval);
    training.ValidationInterval = policy.template Find<int>(config, "validation_interval", defaults.ValidationInterval);
    training.CheckpointInterval = policy.template Find<int>(config, "checkpoint_interval", defaults.CheckpointInterval);
    training.StrengthTestInterval = policy.template Find<int>(config, "strength_test_interval", defaults.StrengthTestInterval);

    training.Stages = policy.template Find<std::vector<StageConfig>>(config, "stages", defaults.Stages);

    training.VocabularyFilename = policy.template Find<std::string>(config, "vocabulary_filename", defaults.VocabularyFilename);
    training.GamesPathSupervised = policy.template Find<std::string>(config, "games_path_supervised", defaults.GamesPathSupervised);
    training.GamesPathTraining = policy.template Find<std::string>(config, "games_path_training", defaults.GamesPathTraining);
    training.GamesPathValidation = policy.template Find<std::string>(config, "games_path_validation", defaults.GamesPathValidation);
    training.CommentaryPathSupervised = policy.template Find<std::string>(config, "commentary_path_supervised", defaults.CommentaryPathSupervised);
    training.CommentaryPathTraining = policy.template Find<std::string>(config, "commentary_path_training", defaults.CommentaryPathTraining);
    training.CommentaryPathValidation = policy.template Find<std::string>(config, "commentary_path_validation", defaults.CommentaryPathValidation);

    return training;
}

template <typename Policy>
SelfPlayConfig ParseSelfPlay(const TomlValue& config, const Policy& policy, const SelfPlayConfig& defaults)
{
    SelfPlayConfig selfPlay;

    selfPlay.NumWorkers = policy.template Find<int>(config, "num_workers", defaults.NumWorkers);
    selfPlay.PredictionBatchSize = policy.template Find<int>(config, "prediction_batch_size", defaults.PredictionBatchSize);

    selfPlay.NumSampingMoves = policy.template Find<int>(config, "num_sampling_moves", defaults.NumSampingMoves);
    selfPlay.MaxMoves = policy.template Find<int>(config, "max_moves", defaults.MaxMoves);
    selfPlay.NumSimulations = policy.template Find<int>(config, "num_simulations", defaults.NumSimulations);

    selfPlay.RootDirichletAlpha = policy.template Find<float>(config, "root_dirichlet_alpha", defaults.RootDirichletAlpha);
    selfPlay.RootExplorationFraction = policy.template Find<float>(config, "root_exploration_fraction", defaults.RootExplorationFraction);

    selfPlay.ExplorationRateBase = policy.template Find<float>(config, "exploration_rate_base", defaults.ExplorationRateBase);
    selfPlay.ExplorationRateInit = policy.template Find<float>(config, "exploration_rate_init", defaults.ExplorationRateInit);

    return selfPlay;
}

MiscConfig ParseMisc(const TomlValue& config)
{
    MiscConfig misc;

    misc.PredictionCache_SizeGb = toml::find<int>(config, "prediction_cache", "size_gb");
    misc.PredictionCache_MaxPly = toml::find<int>(config, "prediction_cache", "max_ply");

    misc.TimeControl_SafetyBufferMs = toml::find<int>(config, "time_control", "safety_buffer_ms");
    misc.TimeControl_FractionOfRemaining = toml::find<int>(config, "time_control", "fraction_remaining");

    misc.Search_MctsParallelism = toml::find<int>(config, "search", "mcts_parallelism");

    misc.Storage_GamesPerChunk = toml::find<int>(config, "storage", "games_per_chunk");

    misc.Paths_Networks = toml::find<std::string>(config, "paths", "networks");
    misc.Paths_TensorBoard = toml::find<std::string>(config, "paths", "tensorboard");
    misc.Paths_Logs = toml::find<std::string>(config, "paths", "logs");
    misc.Paths_Pgns = toml::find<std::string>(config, "paths", "pgns");

    return misc;
}

NetworkConfig Config::TrainingNetwork;
NetworkConfig Config::UciNetwork;
MiscConfig Config::Misc;

void Config::Initialize()
{
    // TODO: Copy to user location
    const std::filesystem::path configTomlPath = Platform::InstallationDataPath() / "config.toml";
    const TomlValue config = toml::parse<toml::discard_comments, std::map, std::vector>(configTomlPath.string());

    // Parse default values.
    const TrainingConfig defaultTraining = ParseTraining(toml::find(config, "training"), DefaultPolicy(), TrainingConfig());
    const SelfPlayConfig defaultSelfPlay = ParseSelfPlay(toml::find(config, "self_play"), DefaultPolicy(), SelfPlayConfig());

    // Parse network configs.
    TrainingNetwork.Name = toml::find<std::string>(config, "network", "training_network_name");
    UciNetwork.Name = toml::find<std::string>(config, "network", "uci_network_name");
    auto networks = { &TrainingNetwork, &UciNetwork };
    const std::vector<TomlValue> configNetworks = toml::find<std::vector<TomlValue>>(config, "networks");
    for (const TomlValue& configNetwork : configNetworks)
    {
        const std::string name = toml::find<std::string>(configNetwork, "name");
        for (NetworkConfig* network : networks)
        {
            if (name == network->Name)
            {
                network->Training = ParseTraining(toml::find_or(configNetwork, "training", TomlValue::table_type()), OverridePolicy(), defaultTraining);
                network->SelfPlay = ParseSelfPlay(toml::find_or(configNetwork, "self_play", TomlValue::table_type()), OverridePolicy(), defaultSelfPlay);
            }
        }
    }

    // Parse miscellaneous config.
    Misc = ParseMisc(config);

    // Apply debug overrides.
#ifdef _DEBUG
    for (NetworkConfig* network : networks)
    {
        network->SelfPlay.NumWorkers = 1;
        network->SelfPlay.PredictionBatchSize = 1;
        if (!network->Training.GamesPathSupervised.empty())
        {
            network->Training.GamesPathSupervised = "Debug/" + network->Training.GamesPathSupervised;
        }
        if (!network->Training.GamesPathTraining.empty())
        {
            network->Training.GamesPathTraining = "Debug/" + network->Training.GamesPathTraining;
        }
        if (!network->Training.GamesPathValidation.empty())
        {
            network->Training.GamesPathValidation = "Debug/" + network->Training.GamesPathValidation;
        }
    }
    // Use the usual networks path, necessary and safe.
    // Same with TensorBoard and Logs for now.
    if (!Misc.Paths_Pgns.empty())
    {
        Misc.Paths_Pgns = "Debug/" + Misc.Paths_Pgns;
    }
#endif
}