// VpScaleRender: renders luma pictures and picture sequences through the real VP renderer DLL to
// an off-screen window of the output size and saves VP's rendered-output captures per picture.
// Used to make the scaling nets' training inputs from VP's own hybrid scaling chain. Data
// preparation only: it never changes VP's deployed configuration.
//
// Usage: VpScaleRender.exe <job.txt>
// job.txt lines (space separated, paths without spaces):
//   dll <VideoProcessorVPRenderer.dll>
//   config <cfg file the renderer loads>
//   shots <capture output folder>
//   source <w> <h> <rateNum> <rateDen>
//   window <w> <h>
//   result <results.txt>
//   log <vp.log>
//   shaders <profile> ...            (shader profiles selected after Build, e.g. standard.ssim_downscaler)
//   require_adapter <name part>      require_hook <glsl file>      nocapture
//   picture <tag> <raw: n frames of w*h uint16 little-endian 10-bit luma codes> <n>
//   picture_seq <tag> <raw> <frames> [first]
//   motion vectors                   (fix/vpscalerender-motion: VP's motion estimate runs; see docs)
//   capture per_frame|after_complete (default per_frame with motion vectors)
//   settle <seconds>                 (default 3 with motion vectors)
//   require_motion <share> [max_invalidations]   (exit 7 when the vectors were not continuous)
// Writes one line per capture:
//   "<captured tag> <frame index> <png> <json> <source_seq> <requested tag> <ok|fail> <hybrid_motion_mode> <motion_estimates> <motion_invalidations>"
// docs/VP-scale-render-motion.md: the modes, the summary line and how the TrainKit calls it.
// Exit codes: 0 ok, 1 a capture failed, 2 bad job, 3/4 renderer not loaded, 6 adapter / hybrid
// passes missing, 7 motion check failed.

#include "pch.h"

#include "VpScaleRenderCore.h"

namespace
{
	void Console(const std::string& line)
	{
		printf("%s\n", line.c_str());
		fflush(stdout);
	}
}

int main(int argc, char** argv)
{
	if (argc < 2) { fprintf(stderr, "usage: VpScaleRender job.txt\n"); return 2; }
	return VpScaleRenderCore::RunJobFile(argv[1], &Console).rc;
}
