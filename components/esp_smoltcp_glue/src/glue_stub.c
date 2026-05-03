/*
 * Stub C compilation unit. Required so idf_component_register has at
 * least one source file to produce a component library; the actual code
 * lives in the Rust staticlib linked via the parent CMakeLists.
 */
const char *smoltcp_glue_build_id(void)
{
    return __DATE__ " " __TIME__;
}
