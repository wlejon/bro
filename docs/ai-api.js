// =============================================================================
// bro.ai, index
// =============================================================================
//
// `bro.ai` has exactly one member: `bro.ai.game`, the brogameagent surface.
// `globalThis.AI` is an alias for it.
//
// The surface is large enough to be split by area. Read the file for the
// thing you are using; each one is annotated against the C++ it binds:
//
//   docs/ai-game-api.js       bro.ai.game navigation
//                             createNavGrid, createHexNav, bakeNavMesh,
//                             loadNavMesh, navMeshAvailable, dynamic
//                             obstacles, off-mesh links, node.navigateTo
//
//   docs/ai-game-planning.js  bro.ai.game simulation
//                             createAgent, createWorld, agent.unit,
//                             bro.ai.game.steer.*, hasLineOfSight, canSee,
//                             computeAim, computeLeadAim, registerCapability,
//                             scene.attachAIWorld, node.attachAgent
//
//   docs/ai-game-learning.js  bro.ai.game search and planning
//                             createMcts and the six other MCTS flavors,
//                             createLayeredPlanner, createCommander, options,
//                             createGenericMcts, buildObservation,
//                             buildActionMask, createRewardTracker,
//                             createSimulation, createRecorder,
//                             createTeamBelief, createInfoSetMcts,
//                             snapshots, projectiles, createVecSimulation
//
//   docs/ai-nn-api.js         bro.ai.game.nn
//                             createLinear / Relu / Tanh / DeepSetsEncoder /
//                             ValueHead / FactoredPolicyHead,
//                             createSingleHeroNet, createPolicyValueNet,
//                             createSingleHeroNetTX, createWeightsHandle,
//                             the free forward/backward ops, factored helpers
//
//   docs/ai-learn-api.js      bro.ai.game.learn
//                             createReplayBuffer, createNeuralEvaluator,
//                             createNeuralPrior, createGumbelNoisePrior,
//                             createExItTrainer, targetsFromMcts,
//                             makeSituation, the generic buffer/trainer,
//                             createInferenceServer, the two backends
//
//   docs/ai-game-tools.js     bro.ai.game.grid
//                             createObsWindow, createFrameStack,
//                             createFailureTape, createBestCrop,
//                             createPotentialShaper, createStallDetector,
//                             generateBC, the .bgargrid recorder/reader,
//                             createGridTrainer
//
// Availability
// ------------
// The navigation, agent, world and search surfaces are present in every
// build and in every mode: windowed `bro`, `bro-headless`, and `bro-server`.
// Inside a Worker each realm installs its own copy of the classes.
//
// Two optional pieces report themselves:
//
//   bro.ai.game.navMeshAvailable          // false without Recast/Detour
//                                         // (-DBROGAMEAGENT_WITH_NAVMESH=ON)
//   bro.ai.game.nn.available === false    // the tensor tower is compiled out,
//   bro.ai.game.learn.available === false // which takes nn, learn and grid
//   bro.ai.game.grid.available === false  // with it
//
// A compiled-IN build does not define `available` at all — only the stub
// namespace sets it — so test `=== false`, not falsiness.
//
// Class globals
// -------------
// Every class is also registered as a global constructor, for `instanceof`
// checks and for debugging: AINavGrid, AIHexNav, AINavMesh, AIAgent,
// AIAgentBinding, AIUnit, AIWorld, AIMcts and the rest of the search classes,
// AILinear / AIPolicyValueNet / ... for the nets, AIReplayBuffer and friends
// for learn, AIGrid* for the grid kit. None of them is callable with `new`;
// objects come from the create*/bake* factories.

if (bro.ai.game.nn.available === false) {
    console.log('this build has no AI tensor tower; nav and search still work');
}

const nav = bro.ai.game.createNavGrid({ minX: -20, minZ: -20, maxX: 20, maxZ: 20 });
const bot = bro.ai.game.createAgent({ navGrid: nav, speed: 6, radius: 0.4 });
const world = bro.ai.game.createWorld();
world.addAgent(bot);
bot.setTarget(10, 5);
world.tick(1 / 60);
console.log(bot.x, bot.z, bot instanceof AIAgent);
