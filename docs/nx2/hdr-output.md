# HDR output

Graphics > "HDR output (Vulkan only)" (`hdr_output`, off by default) shows Switch 2 games in HDR
on an HDR display. Two settings go with it: `hdr_peak_luminance` (1000 nits) and
`hdr_paper_white` (203 nits).

## How it works

1. **Swapchain** (`vk_swapchain.cpp`). With the setting on, the swapchain is created in
   `A2B10G10R10`/`A2R10G10B10` with `VK_COLOR_SPACE_HDR10_ST2084_EXT`, else `R16G16B16A16_SFLOAT`
   with `VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT` (scRGB). The instance enables
   `VK_EXT_swapchain_colorspace`. Windows drivers usually list these only while the display
   has HDR turned on; without them the swapchain stays SDR and the log says so once. With
   `VK_EXT_hdr_metadata`, the swapchain gets BT.2020 mastering metadata with MaxCLL = the peak
   setting.
2. **Guest** (`nx2_gsv.h`). vi2 104 (`QueryDisplayHdrStatus`) answers format 2 (HDR) and the peak
   setting while the host swapchain is HDR (`VideoCommon::HdrOutput`), otherwise format 1 (SDR)
   and 400 nits, as before. 105 answers the paper white setting. Slots the game binds to the HDR
   status (265) and paper white (267) events fire when that state changes (checked every
   vblank), so the game re-queries.
3. **Content.** A game told HDR presents HDR10 (BT.2020, PQ) in its RGB10A2 images (preference
   word 6 = 4). Those presents get `FramebufferConfig::pq_encoded`, and the present frame that
   shows them `Frame::pq_content`. RGBA8 images, and RGB10A2 from a game never told HDR, are
   SDR.
4. **Present.** On an HDR swapchain, present frames are `A2B10G10R10` (`HdrFrameFormat`).
   Instead of the blit,
   `HdrOutputPass` draws the frame into the swapchain image with `vulkan_present_hdr.frag`:
   - HDR10 frame to HDR10 swapchain: copied unchanged.
   - SDR frame to HDR10: gamma 2.2 to linear, times paper white, BT.709 to BT.2020, PQ.
   - Either to scRGB: linear BT.709 at 80 nits per 1.0.
   - HDR10 frame to an SDR swapchain (HDR just turned off, before the game re-queries): PQ to
     linear, BT.2020 to BT.709, divided by paper white, highlights rolled off above 0.8.

   SDR frames on an SDR swapchain take the old blit path unchanged.

## Overrides and logs

- `EDEN_NX2_HDR_CONTENT=pq` treats every RGB10A2 present as HDR10, `=sdr` none of them
  (default: those of a game told HDR).
- Log lines: `Swapchain format ..., colour space ...`, `HDR output active|inactive`,
  `nx2:vi2 104: display HDR status ...`, and `nx2:vi2 composite image N format F (...)`
  with `, HDR10` when the image is treated as HDR10.

Screenshots and `EDEN_NX2_FRAME_CAPTURE` of HDR10 content are tone mapped to SDR on the CPU
(`ToneMapPqScreenshot`, the same curve as the SDR swapchain case) from 8-bit PQ values.

## Known limits

- FXAA, SMAA, FSR and SGSR run on the PQ values of HDR10 content; their intermediates may be
  8-bit.
- Ocarina of Time's upscaled DLSS/FSR presentation (4K, or the output picked in
  `oot_upscale_output`) lays the upscaler's detail over the game's HDR10 output
  (`ocarina_dlss_4k_composite_hdr.comp`), comparing the two in scene units by their local
  brightness ratio. Where they differ (HUD, effects, scene colour above SDR white) the game's 1440p
  output is shown. The log names the shader that drew the HDR10 output, or why the upscaled
  presentation was skipped. Not yet checked on an HDR display.
- Only the Vulkan renderer outputs HDR. OpenGL keeps reporting SDR to games.
- Which images hold HDR10 is inferred from vi2 104 and the RGB10A2 format code; no title has
  been checked on hardware yet. A game that presents HDR in another format code shows as
  `unknown, shown as RGBA8` in the composite log line.
