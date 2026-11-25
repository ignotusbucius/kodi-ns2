/* linktest.c: acceptance (c) for build-deps.sh: libiconv + sqlite3 + OpenSSL link. */
#include <iconv.h>
#include <sqlite3.h>
#include <openssl/crypto.h>
#include <stdio.h>

int main(void)
{
    iconv_t cd = iconv_open("UTF-8", "ISO-8859-1");
    printf("%s | %s | %p\n", sqlite3_libversion(), OpenSSL_version(OPENSSL_VERSION), (void *)cd);
    return 0;
}
