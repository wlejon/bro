# Every dependency bro builds (cmake/bro_deps.cmake), declared before anything
# is added, so as the top-level project these declarations win across the whole
# graph.
#
# Third-party code is pinned to one commit each. Move one by editing its REF.
bro_dependency(SDL GITHUB libsdl-org/SDL REF 1df279a04f604bc50b6f36c9e903b5e64c4fe2a3 THIRD_PARTY PIN_ONLY)
bro_dependency(jolt GITHUB jrouwe/JoltPhysics REF 945d1d5ce29a8ccc8fa78c74059b187c2b67e7d9 THIRD_PARTY PIN_ONLY)
bro_dependency(FastNoise2 GITHUB Auburn/FastNoise2 REF ba93f17ec40a9d09066c8d07b3e72b789e5b5657 THIRD_PARTY PIN_ONLY)

# The ecosystem (github.com/wlejon/<name>) tracks each repo's main: a working
# tree at ../<name> builds as it is, anything else is fetched at its main head
# as of this configure, or at the commit cmake/bro_lock.cmake names when a
# release has locked them (scripts/lock-deps.sh). Declaring them all here lets a
# fresh configure resolve every head in one concurrent batch.
bro_dependencies(
    # The JavaScript toolchain.
    bronze brass
    # Engine libraries.
    bromath htmlayout brokit broimage broaudio bromesh broflora brotensor
    brogameagent brolm brovisionml brodiffusion brosoundml
    # The <terminal> element and remote sessions.
    brosearch brothemes bropty brolink bromux brovideo broremote
    # Desktop substrate.
    broconf brokeys broapps brovfs brothumb broseat brocred brosys brodisplays
    broportal brocompositor browl broa11y brodmabuf brodbus brodecor broclip
    brompris bropulse broime
)
