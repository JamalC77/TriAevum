/* Title-free stand-in for the side-loaded title module: the hello app copies
 * it from <ext>/incoming into filesDir, dlopens it and calls this. */
__attribute__((visibility("default"))) int triaevum_probe(void) { return 42; }
