# Native OpenGL multisample correction

Native Asahi resolves 2x and 4x color attachments to single-sample images with the matching image addressing. Persistent renderbuffers retain their individual samples across flushes and repeated resolves; transient window-system bindings retain direct tile resolves.

Two RGBA8 attachments also retain their individual samples through background reloads and end-of-tile stores. Masked partial clears preserve the uncleared attachment and pixels.

The MRT probe queries both attachments' actual storage sample counts and exercises distinct colors in complementary sample masks across flushes and resolves.

The standalone MSAA probes cover clear/draw with depth, mixed sample colors, reloads, partial rectangles and repeated resolves. Chrome default antialiased WebGL1/WebGL2 presentation and glyph rendering pass on J516S/T6030. This remains experimental native OpenGL support, without a full conformance claim.

The kernel and separate Honeykrisp Vulkan packages retain the release 16 binaries and existing compression settings. M1/M2 and the Air HAL200 profiles retain their existing selection.
