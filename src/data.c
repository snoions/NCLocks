/* Data maintenance layer (design §2.1): the functions are inline in
 * include/ncl/data.h (they sit on every acquire and release). */
#include "ncl/data.h"

struct ncl_data_cfg ncl_dcfg; /* node-local copy */

void ncl_data_configure(const struct ncl_data_cfg *c) { ncl_dcfg = *c; }
