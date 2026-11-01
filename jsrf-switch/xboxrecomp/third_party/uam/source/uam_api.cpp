/* uam.h's C interface over DekoCompiler (jsrf-nx). */
#include "compiler_iface.h"
#include "uam.h"

extern "C" void *uam_compile_dksh(int stage, const char *glsl, uint32_t *size)
{
	DekoCompiler::s_persistent = true;
	DekoCompiler c{stage == UAM_STAGE_FRAGMENT ? pipeline_stage_fragment : pipeline_stage_vertex};
	if (!c.CompileGlsl(glsl))
		return NULL;
	return c.OutputDkshToMemory(size);
}

extern "C" void uam_shutdown(void)
{
	DekoCompiler::Shutdown();
}
