# Every dependency bro builds, pinned to one commit each (cmake/bro_deps.cmake).
#
# Declared before anything is added, so as the top-level project these pins win
# across the whole graph: a sibling that pins the same name for its own
# standalone build is overruled here. A working tree at ../<name> beside bro
# still takes precedence over the pin for local development.
#
# Move a pin with scripts/bump-deps.sh: `scripts/bump-deps.sh brokit` takes
# brokit's origin/main, `--local brokit` the HEAD of ../brokit.

# Third-party code, at the commits the former submodules recorded.
bro_dependency(SDL GITHUB libsdl-org/SDL REF 1df279a04f604bc50b6f36c9e903b5e64c4fe2a3 THIRD_PARTY PIN_ONLY)
bro_dependency(jolt GITHUB jrouwe/JoltPhysics REF 945d1d5ce29a8ccc8fa78c74059b187c2b67e7d9 THIRD_PARTY PIN_ONLY)
bro_dependency(FastNoise2 GITHUB Auburn/FastNoise2 REF ba93f17ec40a9d09066c8d07b3e72b789e5b5657 THIRD_PARTY PIN_ONLY)

# The JavaScript toolchain.
bro_dependency(bronze GITHUB wlejon/bronze REF 8aa8f504ded1d4e5a8393109673a1d2e9d1c04e0 PIN_ONLY)
bro_dependency(brass GITHUB wlejon/brass REF b49d2bde0046fb96b93b95f544d766137dd5563a PIN_ONLY)

# Engine libraries.
bro_dependency(bromath GITHUB wlejon/bromath REF 8b511fbd69c014bf0584d2b42e6c96a8cf720f6a PIN_ONLY)
bro_dependency(htmlayout GITHUB wlejon/htmlayout REF 955b1575be1e9e5a27f3a8a99d0a85b74458a323 PIN_ONLY)
bro_dependency(brokit GITHUB wlejon/brokit REF 23291c709f98d7a30f84a4443502f322a6f99732 PIN_ONLY)
bro_dependency(broimage GITHUB wlejon/broimage REF 9346cd5e97cb2fba0bae15e9f15fc0d9e37349ee PIN_ONLY)
bro_dependency(broaudio GITHUB wlejon/broaudio REF 3b207e85de8721224de6f3fc5598896ee31c6000 PIN_ONLY)
bro_dependency(bromesh GITHUB wlejon/bromesh REF fe7fdc1801f96d246d34e3f33b706ba9359cd217 PIN_ONLY)
bro_dependency(broflora GITHUB wlejon/broflora REF 69b8ead27cb2c0c7d7c2a18d6a7566f8cd91b2a6 PIN_ONLY)
bro_dependency(brotensor GITHUB wlejon/brotensor REF 39fb8e5857af60ba51b8131baf87805bc79b62c0 PIN_ONLY)
bro_dependency(brogameagent GITHUB wlejon/brogameagent REF b037a5e173d866bf8d3dbd73b4510231ca858cf2 PIN_ONLY)
bro_dependency(brolm GITHUB wlejon/brolm REF 3e2547cba802e183196b95523bc53ee99304664b PIN_ONLY)
bro_dependency(brovisionml GITHUB wlejon/brovisionml REF 55727b93a9dbdaf87d86f191fa7c368ec1202ada PIN_ONLY)
bro_dependency(brodiffusion GITHUB wlejon/brodiffusion REF f805fca4c6b0651f775f168514e3d3dbf379f000 PIN_ONLY)
bro_dependency(brosoundml GITHUB wlejon/brosoundml REF 616ffb72758d24000f740873681054592f98d098 PIN_ONLY)

# The <terminal> element and remote sessions.
bro_dependency(brosearch GITHUB wlejon/brosearch REF e408e853181e353e086d547b6d1d3e9cc1c0167f PIN_ONLY)
bro_dependency(brothemes GITHUB wlejon/brothemes REF e993808342a94338a8845664b92631c13128e52a PIN_ONLY)
bro_dependency(bropty GITHUB wlejon/bropty REF d0b7ee2b4ed32e5d0762d7eb0efae7a00094c5c2 PIN_ONLY)
bro_dependency(brolink GITHUB wlejon/brolink REF ddf82bbec31b5251a0a465a5f945ee1c18149242 PIN_ONLY)
bro_dependency(bromux GITHUB wlejon/bromux REF ce5e2862c16afb835414becceefb353986a5c599 PIN_ONLY)
bro_dependency(brovideo GITHUB wlejon/brovideo REF 6182bff5a5c619ba5756615aee99a271969c9ed3 PIN_ONLY)
bro_dependency(broremote GITHUB wlejon/broremote REF e003f664964a48d8f88a8807d75b34452dcb0419 PIN_ONLY)

# Desktop substrate.
bro_dependency(broconf GITHUB wlejon/broconf REF 10c99aee9741dff4108b71378bd25fc6c0a83cb7 PIN_ONLY)
bro_dependency(brokeys GITHUB wlejon/brokeys REF 7542c0c777ae0a458ff33fd7879bc1c5cefc68ca PIN_ONLY)
bro_dependency(broapps GITHUB wlejon/broapps REF e7cfef29316402899db26077419da0ee6e9dd0c6 PIN_ONLY)
bro_dependency(brovfs GITHUB wlejon/brovfs REF 634ee1228e55fd2aaa5d50437b5b571f45e9385d PIN_ONLY)
bro_dependency(brothumb GITHUB wlejon/brothumb REF 1e39a4d156c435c317f6f5584e62f5bf3ed4de80 PIN_ONLY)
bro_dependency(broseat GITHUB wlejon/broseat REF 630444ffff4af680a481c906b2aba4db061fac57 PIN_ONLY)
bro_dependency(brocred GITHUB wlejon/brocred REF 02f9177951f31c7d55e93becf7832709cd805c8d PIN_ONLY)
bro_dependency(brosys GITHUB wlejon/brosys REF c569f713db822e48c5fa6d2cc0eba905db496b96 PIN_ONLY)
bro_dependency(brodisplays GITHUB wlejon/brodisplays REF 8f1bb8b0d2c8c580b5a71d0d34b4b580ed9b2a47 PIN_ONLY)
bro_dependency(broportal GITHUB wlejon/broportal REF 7cf466e94e4a01f017917dbe178390a8bad1a6fc PIN_ONLY)
bro_dependency(brocompositor GITHUB wlejon/brocompositor REF 6cb4ba7e3e488767535eeca2e62324b30742d6c6 PIN_ONLY)
bro_dependency(browl GITHUB wlejon/browl REF 7e0e56ed4ab05dd2e37ac90aa66c2de7fe74137a PIN_ONLY)
bro_dependency(broa11y GITHUB wlejon/broa11y REF 9ef3ade447e6d9971b4f0ad6947a7eb3b264efb7 PIN_ONLY)
bro_dependency(brodmabuf GITHUB wlejon/brodmabuf REF fc84728b39b2172e0312f10714a5332be543f585 PIN_ONLY)
bro_dependency(brodbus GITHUB wlejon/brodbus REF 714ded627aed1c0db4b046d143bcc29e1ec9e9cf PIN_ONLY)
bro_dependency(brodecor GITHUB wlejon/brodecor REF e6c03691d1ed845ebb1c562a0d34b8893fd65c17 PIN_ONLY)
bro_dependency(broclip GITHUB wlejon/broclip REF f552d68da4cae29ff4cd67454f6d9e8c1ad204f5 PIN_ONLY)
bro_dependency(brompris GITHUB wlejon/brompris REF 7d1c26d891ff0f43e870a9f1eed4f97c6f7df100 PIN_ONLY)
bro_dependency(bropulse GITHUB wlejon/bropulse REF 052a947b3d95ddabc4a7c4d0e6071d3b45d6424d PIN_ONLY)
bro_dependency(broime GITHUB wlejon/broime REF 20c592d3ec80cc85f2b9a28a10c151cdba887457 PIN_ONLY)
