/* SPDX-License-Identifier: Apache-2.0
 * 中文：升级元数据边界与版本兼容性。/ English: Release bounds and version compatibility. */
#include "ota_online.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const char *candidate = "{\"schema\":1,\"version\":\"0.3.3-rc79\",\"project\":\"readll\",\"board\":\"RDP-G01-W\",\"layout\":\"readll-dual-4m-v1\",\"minimum_base_version\":\"0.3.3-rc72\",\"url\":\"https://updates.invalid/readll-update-0.3.3-rc79.bin\",\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"notes\":\"Test release\",\"size\":4096}";
int main(void) {
 readll_release_t release;
 assert(readll_release_parse(candidate,strlen(candidate),&release)!=ESP_OK);
 assert(readll_version_compare("0.3.3-rc79","0.3.3-rc9")>0);
 assert(readll_version_compare("0.3.3","0.3.3-rc79")>0);
 assert(readll_version_compare("0.4.0-rc1","0.3.3")>0);
 assert(!readll_release_url_valid("https://updates.invalid/readll-update.bin"));
 assert(!readll_release_url_valid(NULL));
 assert(readll_release_parse(candidate,strlen(candidate)+1,&release)!=ESP_OK);
 assert(readll_release_parse("{}",2,&release)!=ESP_OK);
 char altered[2048]; snprintf(altered,sizeof(altered),"%s junk",candidate);
 assert(readll_release_parse(altered,strlen(altered),&release)!=ESP_OK);
 snprintf(altered,sizeof(altered),"{\"schema\":1,%s",candidate+1);
 assert(readll_release_parse(altered,strlen(altered),&release)!=ESP_OK);
 strcpy(altered,candidate); char *p=strstr(altered,"4096"); memcpy(p,"1e10",4);
 assert(readll_release_parse(altered,strlen(altered),&release)!=ESP_OK);
 strcpy(altered,candidate); p=strstr(altered,"RDP-G01-W"); p[0]='X';
 assert(readll_release_parse(altered,strlen(altered),&release)!=ESP_OK);
 strcpy(altered,candidate); p=strstr(altered,"0123456789abcdef"); p[0]='z';
 assert(readll_release_parse(altered,strlen(altered),&release)!=ESP_OK);
 puts("PASS: version ordering remains available while every unconfigured online release origin is rejected");
}
