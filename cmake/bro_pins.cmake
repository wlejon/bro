# Every dependency bro builds (cmake/bro_deps.cmake), declared before anything
# is added, so as the top-level project these declarations win across the whole
# graph.
#
# Third-party code is pinned to one commit each. Move one by editing its REF.
bro_dependency(SDL GITHUB libsdl-org/SDL REF 1df279a04f604bc50b6f36c9e903b5e64c4fe2a3 THIRD_PARTY PIN_ONLY)
bro_dependency(jolt GITHUB jrouwe/JoltPhysics REF 945d1d5ce29a8ccc8fa78c74059b187c2b67e7d9 THIRD_PARTY PIN_ONLY)
# broimage's JPEG decoder, built from this source only where no installed one
# (system package, Homebrew, vcpkg) is found: 3.2.0, as broimage's
# cmake/jpeg_turbo.cmake pins it.
bro_dependency(libjpeg-turbo GITHUB libjpeg-turbo/libjpeg-turbo REF c85e6b905bf237038faa936dab160ebfc5da0344 THIRD_PARTY PIN_ONLY)

# The ecosystem (github.com/wlejon/<name>) tracks each repo's main: a working
# tree at ../<name> builds as it is, anything else is fetched at its main head
# as of this configure, or at the commit cmake/bro_lock.cmake names when a
# release has locked them (scripts/lock-deps.sh). Declaring them all here lets a
# fresh configure resolve every head in one concurrent batch.
bro_dependencies(
    # The JavaScript and Lua toolchain.
    bronze brass brasslua
    # Engine libraries (FastNoise2 is the fork of Auburn's, its positions far
    # from the origin exact).
    bromath htmlayout brokit broimage broaudio bromesh broflora brotensor
    brogameagent brolm brovisionml brodiffusion brosoundml FastNoise2
    # The <terminal> element and remote sessions.
    brosearch brothemes bropty brolink bromux brovideo broremote
    # Desktop substrate.
    broconf brokeys broapps brovfs brothumb broseat brocred brosys brodisplays
    broportal brocompositor browl broa11y brodmabuf brodbus brodecor broclip
    brompris bropulse broime
)
