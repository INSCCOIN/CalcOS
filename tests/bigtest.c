/* The big-integer core, driven from a script and checked against Python.
 *
 *   bigtest add A B      prints A+B
 *   bigtest sub|mul|div|mod|gcd A B
 *   bigtest pow A E
 *   bigtest powmod A E M
 *   bigtest fact N
 *   bigtest cmp A B      prints -1, 0 or 1
 *   bigtest shl|shr A K
 *   bigtest roundtrip A  parses and prints it again
 *
 * Division is the part that is easy to get subtly wrong, so the suite runs
 * a few thousand random cases through this and compares every one against
 * Python's own integers.
 */
#include "bignum.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char out[BIG_MAXDIGITS + 8];

static void emit(Big *b)
{
    if (!b) { printf("ERR\n"); return; }
    big_str(b, out, (int)sizeof out);
    printf("%s\n", out);
    big_free(b);
}

int main(int argc, char **argv)
{
    Big *a = NULL, *b = NULL, *c = NULL;
    const char *op;

    if (argc < 3) { fprintf(stderr, "usage: bigtest OP A [B]\n"); return 2; }
    op = argv[1];

    if (!strcmp(op, "fact")) {
        emit(big_fact(atoll(argv[2])));
        return 0;
    }

    a = big_parse(argv[2]);
    if (!a) { printf("ERR\n"); return 0; }

    if (!strcmp(op, "roundtrip")) { emit(a); return 0; }
    if (!strcmp(op, "digits")) { printf("%d\n", big_bits(a)); big_free(a); return 0; }
    if (!strcmp(op, "todouble")) { printf("%.17g\n", big_to_double(a)); big_free(a); return 0; }

    if (argc < 4) { big_free(a); fprintf(stderr, "needs two operands\n"); return 2; }

    if (!strcmp(op, "pow"))  { emit(big_pow(a, atoll(argv[3]))); big_free(a); return 0; }
    if (!strcmp(op, "shl"))  { emit(big_shl(a, atoi(argv[3]))); big_free(a); return 0; }
    if (!strcmp(op, "shr"))  { emit(big_shr(a, atoi(argv[3]))); big_free(a); return 0; }

    b = big_parse(argv[3]);
    if (!b) { big_free(a); printf("ERR\n"); return 0; }

    if (!strcmp(op, "powmod")) {
        if (argc < 5) { printf("ERR\n"); }
        else {
            c = big_parse(argv[4]);
            emit(c ? big_powmod(a, b, c) : NULL);
            big_free(c);
        }
    }
    else if (!strcmp(op, "add")) emit(big_add(a, b));
    else if (!strcmp(op, "sub")) emit(big_sub(a, b));
    else if (!strcmp(op, "mul")) emit(big_mul(a, b));
    else if (!strcmp(op, "div")) emit(big_div(a, b));
    else if (!strcmp(op, "mod")) emit(big_mod(a, b));
    else if (!strcmp(op, "gcd")) emit(big_gcd(a, b));
    else if (!strcmp(op, "cmp")) printf("%d\n", big_cmp(a, b));
    else { fprintf(stderr, "unknown op %s\n", op); big_free(a); big_free(b); return 2; }

    big_free(a);
    big_free(b);
    return 0;
}
