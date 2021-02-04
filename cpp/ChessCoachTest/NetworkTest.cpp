#include <gtest/gtest.h>

#include <algorithm>

#pragma warning(disable:4100) // Ignore unused args in generated code
#pragma warning(disable:4127) // Ignore const-per-architecture warning
#include <protobuf/ChessCoach.pb.h>
#pragma warning(default:4127) // Ignore const-per-architecture warning
#pragma warning(default:4100) // Ignore unused args in generated code

#include <ChessCoach/SelfPlay.h>
#include <ChessCoach/ChessCoach.h>

void ApplyMoveExpandWithPattern(SelfPlayGame& game, Move move, int patternIndex)
{
    const MoveList legalMoves = MoveList<LEGAL>(game.GetPosition());
    const int legalMoveCount = static_cast<int>(legalMoves.size());

    int moveIndex = 0;
    Node* moveNode = nullptr;
    game.Root()->childCount = static_cast<int>(legalMoves.size());
    game.Root()->children = new Node[game.Root()->childCount]{};
    for (Move legalMove : legalMoves)
    {
        Node* child = &game.Root()->children[moveIndex];
        child->move = static_cast<uint16_t>(legalMove);
        if (move == legalMove)
        {
            moveNode = child;
        }

        const int visitCount = (((moveIndex + patternIndex) % legalMoveCount) + 1);
        child->visitCount += visitCount;
        game.Root()->visitCount += visitCount;

        ASSERT_LT(patternIndex, 32);
        const float value = (patternIndex / 32.f);
        child->valueAverage += value;
        child->valueWeight += visitCount;
        game.Root()->valueWeight += visitCount;
        game.Root()->valueAverage += visitCount * (Game::FlipValue(value) - game.Root()->valueAverage) / game.Root()->valueWeight; // Arithmetic mean here
        ASSERT_GE(game.Root()->Value(), 0.f);
        ASSERT_LE(game.Root()->Value(), 1.f);
        moveIndex++;
    }
    ASSERT_NE(moveNode, nullptr);

    Node* previousRoot = game.Root();
    game.StoreSearchStatistics();
    game.ApplyMoveWithRootAndHistory(move, moveNode);
    game.PruneExcept(previousRoot, moveNode);
}
TEST(Network, Policy)
{
    ChessCoach chessCoach;
    chessCoach.Initialize();

    // Just use training network config.
    const NetworkConfig& networkConfig = Config::TrainingNetwork;

    std::vector<INetwork::InputPlanes> images(networkConfig.SelfPlay.PredictionBatchSize);
    std::vector<float> values(networkConfig.SelfPlay.PredictionBatchSize);
    std::vector<INetwork::OutputPlanes> policies(networkConfig.SelfPlay.PredictionBatchSize);

    SelfPlayGame game("3rkb1r/p2nqppp/5n2/1B2p1B1/4P3/1Q6/PPP2PPP/2KR3R w k - 3 13", {}, false /* tryHard */, &images[0], &values[0], &policies[0]);
    
    MoveList legalMoves = MoveList<LEGAL>(game.GetPosition());
    const int legalMoveCount = static_cast<int>(legalMoves.size());

    // Give 5 visits evenly across legal moves, then the rest to the first move.
    game.Root()->childCount = static_cast<int>(legalMoves.size());
    game.Root()->children = new Node[game.Root()->childCount]{};
    const int evenCount = 5;
    int moveIndex = 0;
    for (Move move : legalMoves)
    {
        Node* child = &game.Root()->children[moveIndex++];
        child->move = static_cast<uint16_t>(move);
        child->visitCount += evenCount;
    }
    Move firstMove = *legalMoves.begin();
    Node* selected = &game.Root()->children[0];
    selected->visitCount += (networkConfig.SelfPlay.NumSimulations - (legalMoveCount * evenCount));
    game.Root()->visitCount = networkConfig.SelfPlay.NumSimulations;

    // Generate policy labels. Make sure that legal moves are non-zero and the rest are zero.
    Node* previousRoot = game.Root();
    game.StoreSearchStatistics();
    game.ApplyMoveWithRootAndHistory(firstMove, selected);
    game.PruneExcept(previousRoot, selected);
    game.Complete();
    std::unique_ptr<INetwork::OutputPlanes> labels(std::make_unique<INetwork::OutputPlanes>());
    game.GeneratePolicy(game.Save().childVisits[0], *labels);
    
    for (Move move : legalMoves)
    {
        EXPECT_GT(game.PolicyValue(*labels, move), 0.f);
    }

    int zeroCount = 0;
    INetwork::PlanesPointerFlat value = reinterpret_cast<INetwork::PlanesPointerFlat>(labels.get());
    for (int i = 0; i < INetwork::OutputPlanesFloatCount; i++)
    {
        if (*(value++) == 0.f) zeroCount++;
    }
    EXPECT_EQ(zeroCount, INetwork::OutputPlanesFloatCount - legalMoveCount);

    // This isn't really a test, just checking some ballpark loss (~3.93).
    // Check categorical cross-entropy loss. Fake a uniform policy, post-softmax.
    float prediction = (1.f / legalMoveCount);
    float sum = 0;
    for (int i = 0; i < legalMoveCount; i++)
    {
        sum += (game.PolicyValue(*labels, *(legalMoves.begin() + i)) * -::logf(prediction));
    }
    EXPECT_LT(sum, 5.f);
}

TEST(Network, ImagePieceHistoryPlanes)
{
    ChessCoach chessCoach;
    chessCoach.Initialize();

    Game game;

    const int finalHistoryPlanes = (INetwork::InputPreviousPositionCount - 1) * INetwork::InputPiecePlanesPerPosition;
    const int currentPositionPlanes = INetwork::InputPreviousPositionCount * INetwork::InputPiecePlanesPerPosition;

    // Ensure that the final history plane is all zeros.
    std::unique_ptr<INetwork::InputPlanes> image1(std::make_unique<INetwork::InputPlanes>());
    game.GenerateImage(*image1);
    const INetwork::PackedPlane startingPositionOurPawns = (*image1)[currentPositionPlanes + 0];
    for (int i = 0; i < INetwork::InputPiecePlanesPerPosition; i++)
    {
        EXPECT_EQ((*image1)[finalHistoryPlanes + i], 0);
    }

    // Make a move. Ensure that the final history our-pawns plane equals the starting position's.
    game.ApplyMove(make_move(SQ_E2, SQ_E4));
    std::unique_ptr<INetwork::InputPlanes> image2(std::make_unique<INetwork::InputPlanes>());
    game.GenerateImage(*image2);
    EXPECT_EQ((*image2)[finalHistoryPlanes + 0], startingPositionOurPawns);
}

TEST(Network, CompressDecompress)
{
    ChessCoach chessCoach;
    chessCoach.Initialize();

    SelfPlayGame game(nullptr, nullptr, nullptr);

    // Play some moves, generating mostly different policy distributions for each move.
    const std::vector<Move> moves =
    {
        make_move(SQ_E2, SQ_E4), make_move(SQ_E7, SQ_E5), make_move(SQ_G1, SQ_F3), make_move(SQ_D7, SQ_D6),
        make_move(SQ_D2, SQ_D4), make_move(SQ_C8, SQ_G4), make_move(SQ_D4, SQ_E5), make_move(SQ_G4, SQ_F3),
        make_move(SQ_D1, SQ_F3), make_move(SQ_D6, SQ_E5), make_move(SQ_F1, SQ_C4), make_move(SQ_G8, SQ_F6),
    };
    for (int i = 0; i < moves.size(); i++)
    {
        ApplyMoveExpandWithPattern(game, moves[i], i);
    }
    game.Root()->terminalValue = TerminalValue::MateIn<1>(); // Fudge a non-draw so that flips are interesting.
    game.Complete();
    const SavedGame savedGame = game.Save();

    // Generate compressed training tensors.
    const Storage storage(Config::TrainingNetwork, Config::Misc);
    message::Example compressed = storage.DebugPopulateGame(savedGame);

    // Decompress in Python.
    auto& features = *compressed.mutable_features()->mutable_feature();
    auto& result = *features["result"].mutable_float_list()->mutable_value();
    auto& mctsValues = *features["mcts_values"].mutable_float_list()->mutable_value();
    auto& imagePiecesAuxiliary = *features["image_pieces_auxiliary"].mutable_int64_list()->mutable_value();
    auto& policyRowLengths = *features["policy_row_lengths"].mutable_int64_list()->mutable_value();
    auto& policyIndices = *features["policy_indices"].mutable_int64_list()->mutable_value();
    auto& policyValues = *features["policy_values"].mutable_float_list()->mutable_value();

    std::vector<INetwork::InputPlanes> images(savedGame.moveCount);
    std::vector<float> values(savedGame.moveCount);
    std::vector<INetwork::OutputPlanes> policies(savedGame.moveCount);

    const int decompressPositionsModulus = 1; // Every position
    std::unique_ptr<INetwork> network(chessCoach.CreateNetwork(Config::TrainingNetwork));
    network->DebugDecompress(savedGame.moveCount, policyIndices.size(), result.mutable_data(), imagePiecesAuxiliary.mutable_data(),
        policyRowLengths.mutable_data(), policyIndices.mutable_data(), policyValues.mutable_data(), decompressPositionsModulus,
        images.data(), values.data(), policies.data());

    // Generate full training tensors to compare.
    Game scratchGame;
    for (int i = 0; i < moves.size(); i++)
    {
        INetwork::InputPlanes image;
        float value;
        INetwork::OutputPlanes policy{}; // "GeneratePolicy" requires zeroed planes.

        scratchGame.GenerateImage(image);

        value = Game::FlipValue(scratchGame.ToPlay(), savedGame.result);

        scratchGame.GeneratePolicy(savedGame.childVisits[i], policy);

        // Compare compressed to uncompressed.
        EXPECT_EQ(image, images[i]);
        EXPECT_EQ(value, values[i]);
        EXPECT_EQ(policy, policies[i]);

        // Sanity-check.
        image[5] += 7;
        value += 0.0000005f;
        policy[5][3][2] += 0.00000025f;
        EXPECT_NE(image, images[i]);
        EXPECT_NE(value, values[i]);
        EXPECT_NE(policy, policies[i]);

        scratchGame.ApplyMove(moves[i]);
    }

    // More sanity-checks.
    EXPECT_NE(mctsValues[0], mctsValues[1]);
    static_assert(INetwork::InputPreviousPositionCount == 7);
    EXPECT_EQ(images[6][0], 0);
    EXPECT_NE(images[7][0], 0);
}

TEST(Network, CompressDecompressSparse)
{
    ChessCoach chessCoach;
    chessCoach.Initialize();

    SelfPlayGame game(nullptr, nullptr, nullptr);

    // Play some moves, generating mostly different policy distributions for each move.
    const std::vector<Move> moves =
    {
        make_move(SQ_E2, SQ_E4), make_move(SQ_E7, SQ_E5), make_move(SQ_G1, SQ_F3), make_move(SQ_D7, SQ_D6),
        make_move(SQ_D2, SQ_D4), make_move(SQ_C8, SQ_G4), make_move(SQ_D4, SQ_E5), make_move(SQ_G4, SQ_F3),
        make_move(SQ_D1, SQ_F3), make_move(SQ_D6, SQ_E5), make_move(SQ_F1, SQ_C4), make_move(SQ_G8, SQ_F6),
    };
    for (int i = 0; i < moves.size(); i++)
    {
        ApplyMoveExpandWithPattern(game, moves[i], i);
    }
    game.Root()->terminalValue = TerminalValue::MateIn<1>(); // Fudge a non-draw so that flips are interesting.
    game.Complete();
    const SavedGame savedGame = game.Save();

    // Generate compressed training tensors.
    const Storage storage(Config::TrainingNetwork, Config::Misc);
    message::Example compressed = storage.DebugPopulateGame(savedGame);

    // Decompress in Python.
    auto& features = *compressed.mutable_features()->mutable_feature();
    auto& result = *features["result"].mutable_float_list()->mutable_value();
    auto& imagePiecesAuxiliary = *features["image_pieces_auxiliary"].mutable_int64_list()->mutable_value();
    auto& policyRowLengths = *features["policy_row_lengths"].mutable_int64_list()->mutable_value();
    auto& policyIndices = *features["policy_indices"].mutable_int64_list()->mutable_value();
    auto& policyValues = *features["policy_values"].mutable_float_list()->mutable_value();

    std::vector<INetwork::InputPlanes> images(savedGame.moveCount);
    std::vector<float> values(savedGame.moveCount);
    std::vector<INetwork::OutputPlanes> policies(savedGame.moveCount);

    const int decompressPositionsModulus = 3; // Every 3rd position
    std::unique_ptr<INetwork> network(chessCoach.CreateNetwork(Config::TrainingNetwork));
    network->DebugDecompress(savedGame.moveCount, policyIndices.size(), result.mutable_data(), imagePiecesAuxiliary.mutable_data(),
        policyRowLengths.mutable_data(), policyIndices.mutable_data(), policyValues.mutable_data(), decompressPositionsModulus,
        images.data(), values.data(), policies.data());

    // Generate full training tensors to compare.
    Game scratchGame;
    for (int i = 0; i < moves.size(); i++)
    {
        INetwork::InputPlanes image;
        float value;
        INetwork::OutputPlanes policy{}; // "GeneratePolicy" requires zeroed planes.

        scratchGame.GenerateImage(image);

        value = Game::FlipValue(scratchGame.ToPlay(), savedGame.result);

        scratchGame.GeneratePolicy(savedGame.childVisits[i], policy);

        // Compare compressed to uncompressed.
        if (i % decompressPositionsModulus == 0)
        {
            EXPECT_EQ(image, images[i / decompressPositionsModulus]);
            EXPECT_EQ(value, values[i / decompressPositionsModulus]);
            EXPECT_EQ(policy, policies[i / decompressPositionsModulus]);
        }

        // Don't repeat sanity-checks.

        scratchGame.ApplyMove(moves[i]);
    }

    // Don't repeat sanity-checks.
}

TEST(Network, QueenKnightPlanes)
{
    ChessCoach chessCoach;
    chessCoach.Initialize();

    float policySums[COLOR_NB] = {};
    for (int i = 0; i < COLOR_NB; i++)
    {
        Color toPlay = Color(i);
        Game game;
        INetwork::OutputPlanes policy{}; // Zero the policy planes.

        if (toPlay == BLACK)
        {
            game.ApplyMoveMaybeNull(MOVE_NULL);
        }
        EXPECT_EQ(game.ToPlay(), toPlay);

        // Increment the policy value for all queen and knight moves.
        Position position{}; // Zero the position, since no set() call.
        for (Square from = SQ_A1; from <= SQ_H8; ++from)
        {
            Bitboard queenMoves = position.attacks_from(QUEEN, from);
            Bitboard knightMoves = position.attacks_from(KNIGHT, from);

            while (queenMoves)
            {
                const Square to = pop_lsb(&queenMoves);
                game.PolicyValue(policy, make_move(from, to)) += 1.f;
            }
            while (knightMoves)
            {
                const Square to = pop_lsb(&knightMoves);
                game.PolicyValue(policy, make_move(from, to)) += 1.f;
            }
        }

        // Increment the policy value for all underpromotions, excluding the 2x3 illegal pseudo-possibilities, axz and hxi.
        for (Square from = SQ_A7; from <= SQ_H7; ++from)
        {
            if (file_of(from) > FILE_A)
            {
                for (PieceType pieceType = KNIGHT; pieceType <= ROOK; ++pieceType)
                {
                    game.PolicyValue(policy, Game::FlipMove(toPlay, make<PROMOTION>(from, from + NORTH_WEST, pieceType))) += 1.f;
                }
            }
            {
                for (PieceType pieceType = KNIGHT; pieceType <= ROOK; ++pieceType)
                {
                    game.PolicyValue(policy, Game::FlipMove(toPlay, make<PROMOTION>(from, from + NORTH, pieceType))) += 1.f;
                }
            }
            if (file_of(from) < FILE_H)
            {
                for (PieceType pieceType = KNIGHT; pieceType <= ROOK; ++pieceType)
                {
                    game.PolicyValue(policy, Game::FlipMove(toPlay, make<PROMOTION>(from, from + NORTH_EAST, pieceType))) += 1.f;
                }
            }
        }

        // Check for colliding policy values.
        INetwork::PlanesPointerFlat policyFlat = reinterpret_cast<INetwork::PlanesPointerFlat>(policy.data());
        for (int j = 0; j < INetwork::OutputPlanesFloatCount; j++)
        {
            const float policyValue = policyFlat[j];
            policySums[toPlay] += policyValue;
            EXPECT_GE(policyValue, 0.f);
            EXPECT_LE(policyValue, 1.f);
        }
    }

    EXPECT_EQ(policySums[WHITE], policySums[BLACK]);
}

TEST(Network, NullMoveFlip)
{
    ChessCoach chessCoach;
    chessCoach.Initialize();

    // Set up a position and generate image planes.
    Game game("3rkb1r/p2nqppp/5n2/1B2p1B1/4P3/1Q6/PPP2PPP/2KR3R w k - 3 13", {});
    INetwork::InputPlanes image1;
    game.GenerateImage(image1);

    // Apply a null move (e.g. like in a commentary training variation) and generate image planes.
    game.ApplyMoveMaybeNull(MOVE_NULL);
    INetwork::InputPlanes image2;
    game.GenerateImage(image2);

    // Expect that the piece planes for the "current" position are identical but flipped.
    for (int i = 0; i < INetwork::INetwork::InputPiecePlanesPerPosition; i++)
    {
        const int historyPlanes = (INetwork::InputPreviousPositionCount * INetwork::InputPiecePlanesPerPosition);
        const int ourPieces = (i + historyPlanes);
        const int theirPieces = (((i + INetwork::INetwork::InputPiecePlanesPerPosition / 2) % INetwork::INetwork::InputPiecePlanesPerPosition) + historyPlanes);
        const INetwork::PackedPlane original = image1[ourPieces];
        const INetwork::PackedPlane nullFlipTheirs = Game::FlipBoard(image2[theirPieces]);
        EXPECT_EQ(original, nullFlipTheirs);
    }
}