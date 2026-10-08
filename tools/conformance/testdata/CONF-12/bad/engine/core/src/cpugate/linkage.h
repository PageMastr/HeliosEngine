#ifdef __cplusplus
extern "C" {
#endif
int hcg_in_linkage_block;
unsigned hcg_fn_in_linkage_block(void) { return 0; }
#ifdef __cplusplus
}
extern "C++" {
int hcg_in_cxx_linkage_block;
}
#endif
int hcg_after_linkage_block;
