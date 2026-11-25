/*
 * hashpw.c — 사용자 파일에 넣을 비밀번호 해시(argon2id) 만들기
 *
 *   printf '%s' 'password' | build/hashpw     →  $argon2id$v=19$m=...
 *
 * 비밀번호를 명령줄 인자로 받지 않는 이유: ps나 셸 기록에 남기 때문.
 */
#include <sodium.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char password[256];
    size_t length = fread(password, 1, sizeof(password) - 1, stdin);
    password[length] = '\0';
    char hash[crypto_pwhash_STRBYTES];
    if (sodium_init() < 0 ||
        crypto_pwhash_str(hash, password, length, crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        fprintf(stderr, "hash failed\n");
        return 1;
    }
    sodium_memzero(password, sizeof(password));
    puts(hash);
    return 0;
}
