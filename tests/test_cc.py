#!/usr/bin/env python3
"""End-to-end tests for the CalcOS C compiler.

Each case compiles real C, runs the generated Thumb-2 on an emulated
Cortex-M33 and checks the answer. Nothing here is a parser-only check:
if it passes, the machine code was executed.
"""
import sys, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ccemu import run_source

CASES = []       # (label, source, fn, args, expected, kind)


def case(label, src, fn="f", args=(), want=None, kind="int"):
    CASES.append((label, src, fn, args, want, kind))


# ---------------------------------------------------------------- integers
case("arithmetic", "int f(){ return 2+3*4-6/2; }", want=11)
case("precedence", "int f(){ return (2+3)*(4-1); }", want=15)
case("modulo", "int f(){ return 17%5; }", want=2)
case("negative", "int f(){ return -7/2; }", want=-3)
case("bitwise", "int f(){ return (0xF0|0x0F)&0xFF ^ 0x0F; }", want=0xF0)
case("shifts", "int f(){ return (1<<10) + (1024>>3); }", want=1152)
case("unsigned shift",
     "int f(){ unsigned x = 0x80000000; return (int)(x>>28); }", want=8)
case("comparison", "int f(){ return (3<4) + (5>=5)*2 + (1!=1); }", want=3)
case("logical and", "int f(){ return (1&&0) + (1&&2)*10; }", want=10)
case("logical or", "int f(){ return (0||0) + (0||5)*10; }", want=10)
case("short circuit",
     "int n; int bump(){ n=1; return 1; } int f(){ n=0; 0 && bump(); return n; }",
     want=0)
case("ternary", "int f(){ int a=5; return a>3 ? 100 : 200; }", want=100)
case("not", "int f(){ return !0 + !5; }", want=1)
case("complement", "int f(){ return ~0; }", want=-1)
case("comma", "int f(){ int a; a=(1,2,3); return a; }", want=3)

# ---------------------------------------------------------------- control
case("if else", "int f(int x){ if(x>0) return 1; else return -1; }",
     args=(5,), want=1)
case("if else neg", "int f(int x){ if(x>0) return 1; else return -1; }",
     args=(-5,), want=-1)
case("while", "int f(int n){ int s=0; while(n>0){ s+=n; n--; } return s; }",
     args=(10,), want=55)
case("do while", "int f(){ int i=0; int s=0; do { s+=i; i++; } while(i<5); return s; }",
     want=10)
case("for", "int f(int n){ int s=0; int i; for(i=0;i<n;i++) s+=i*i; return s; }",
     args=(10,), want=285)
case("nested loops",
     "int f(){ int i,j,s=0; for(i=0;i<10;i++) for(j=0;j<10;j++) s++; return s; }",
     want=100)
case("break", "int f(){ int i,s=0; for(i=0;i<100;i++){ if(i==10) break; s+=i; } return s; }",
     want=45)
case("continue",
     "int f(){ int i,s=0; for(i=0;i<10;i++){ if(i%2) continue; s+=i; } return s; }",
     want=20)
case("switch",
     "int f(int x){ switch(x){ case 1: return 10; case 2: return 20; "
     "default: return 99; } }", args=(2,), want=20)
case("switch default",
     "int f(int x){ switch(x){ case 1: return 10; default: return 99; } }",
     args=(7,), want=99)
case("switch fallthrough",
     "int f(int x){ int s=0; switch(x){ case 1: s+=1; case 2: s+=2; case 3: s+=4; } return s; }",
     args=(1,), want=7)
case("switch break",
     "int f(int x){ int s=0; switch(x){ case 1: s=1; break; case 2: s=2; break; } return s; }",
     args=(2,), want=2)
case("goto", "int f(){ int i=0; loop: i++; if(i<5) goto loop; return i; }", want=5)
case("recursion", "int fib(int n){ return n<2?n:fib(n-1)+fib(n-2); } "
     "int f(){ return fib(20); }", want=6765)
case("mutual recursion",
     "int odd(int n); int even(int n){ return n==0?1:odd(n-1); } "
     "int odd(int n){ return n==0?0:even(n-1); } int f(){ return even(10); }",
     want=1)

# ---------------------------------------------------------------- pointers
case("pointer deref", "int f(){ int x=42; int *p=&x; return *p; }", want=42)
case("pointer write", "int f(){ int x=1; int *p=&x; *p=99; return x; }", want=99)
case("pointer arith",
     "int f(){ int a[5]; int *p=a; a[0]=1; a[1]=2; a[2]=3; return *(p+2); }",
     want=3)
case("pointer diff",
     "int f(){ int a[5]; int *p=&a[4]; int *q=&a[1]; return p-q; }", want=3)
case("array index", "int f(){ int a[4]; a[0]=10; a[3]=40; return a[0]+a[3]; }",
     want=50)
case("array loop",
     "int f(){ int a[10]; int i,s=0; for(i=0;i<10;i++) a[i]=i*i; "
     "for(i=0;i<10;i++) s+=a[i]; return s; }", want=285)
case("array init", "int f(){ int a[4] = {5,6,7,8}; return a[2]; }", want=7)
case("2d array",
     "int f(){ int a[3][4]; int i,j; for(i=0;i<3;i++) for(j=0;j<4;j++) a[i][j]=i*10+j;"
     " return a[2][3]; }", want=23)
case("char array",
     "int f(){ char s[8]; s[0]='A'; s[1]=0; return s[0]; }", want=65)
case("string literal", 'int f(){ char *s = "hello"; return s[1]; }', want=ord('e'))
case("string index", 'int f(){ return "abcdef"[3]; }', want=ord('d'))
case("global var", "int g; int f(){ g = 7; return g*6; }", want=42)
case("global init", "int g = 123; int f(){ return g; }", want=123)
case("global array", "int a[3] = {4,5,6}; int f(){ return a[0]+a[1]+a[2]; }",
     want=15)
case("pointer to pointer",
     "int f(){ int x=5; int *p=&x; int **q=&p; return **q; }", want=5)

# ---------------------------------------------------------------- increments
case("post increment", "int f(){ int i=5; int j=i++; return j*10+i; }", want=56)
case("pre increment", "int f(){ int i=5; int j=++i; return j*10+i; }", want=66)
case("post decrement", "int f(){ int i=5; int j=i--; return j*10+i; }", want=54)
case("pointer increment",
     "int f(){ int a[3]; a[0]=1;a[1]=2;a[2]=3; int *p=a; p++; return *p; }",
     want=2)
case("compound add", "int f(){ int x=10; x+=5; return x; }", want=15)
case("compound mul", "int f(){ int x=10; x*=3; return x; }", want=30)
case("compound shift", "int f(){ int x=1; x<<=8; return x; }", want=256)
case("compound on array",
     "int f(){ int a[2]; a[0]=10; a[0]+=5; return a[0]; }", want=15)

# ---------------------------------------------------------------- structs
case("struct member",
     "struct P { int x; int y; }; int f(){ struct P p; p.x=3; p.y=4; return p.x*p.y; }",
     want=12)
case("struct pointer",
     "struct P { int x; int y; }; int f(){ struct P p; struct P *q=&p; "
     "q->x=5; q->y=6; return q->x+q->y; }", want=11)
case("struct array",
     "struct P { int x; int y; }; int f(){ struct P a[3]; int i; "
     "for(i=0;i<3;i++){ a[i].x=i; a[i].y=i*2; } return a[2].x+a[2].y; }",
     want=6)
case("struct size",
     "struct P { int x; char c; int y; }; int f(){ return sizeof(struct P); }",
     want=12)
case("struct assign",
     "struct P { int x; int y; }; int f(){ struct P a; struct P b; "
     "a.x=1; a.y=2; b=a; return b.x+b.y*10; }", want=21)
case("nested struct",
     "struct In { int v; }; struct Out { struct In i; int w; }; "
     "int f(){ struct Out o; o.i.v=7; o.w=3; return o.i.v*o.w; }", want=21)
case("union",
     "union U { int i; char c[4]; }; int f(){ union U u; u.i=0; u.c[0]=1; "
     "u.c[1]=1; return u.i; }", want=257)
case("struct init",
     "struct P { int x; int y; }; int f(){ struct P p = {3,4}; return p.x*p.y; }",
     want=12)

# ---------------------------------------------------------------- types
case("char narrowing", "int f(){ char c = 300; return c; }", want=44)
case("unsigned char", "int f(){ unsigned char c = 300; return c; }", want=44)
case("char signed", "int f(){ char c = -1; return c; }", want=-1)
case("short", "int f(){ short s = 70000; return s; }", want=4464)
case("sizeof types",
     "int f(){ return sizeof(char)+sizeof(short)*10+sizeof(int)*100"
     "+sizeof(double)*1000; }", want=1+20+400+8000)
case("sizeof array", "int f(){ int a[7]; return sizeof(a); }", want=28)
case("cast int", "int f(){ double d = 3.9; return (int)d; }", want=3)
case("cast double", "int f(){ int i = 7; double d = (double)i/2; return (int)(d*10); }",
     want=35)
case("typedef", "typedef int myint; int f(){ myint x = 5; return x*2; }", want=10)
case("enum", "enum { A, B, C=10, D }; int f(){ return A+B*2+C*4+D*8; }",
     want=0 + 2 + 40 + 88)

# ---------------------------------------------------------------- doubles
case("double add", "double f(){ return 1.5 + 2.25; }", want=3.75, kind="dbl")
case("double mul", "double f(){ return 2.5 * 4.0; }", want=10.0, kind="dbl")
case("double div", "double f(){ return 1.0 / 3.0; }", want=1.0/3.0, kind="dbl")
case("double mixed", "double f(){ return 1 + 0.5; }", want=1.5, kind="dbl")
case("double arg", "double f(double x){ return x*x; }", args=(3.0,), want=9.0,
     kind="dbl")
case("double two args", "double f(double a, double b){ return a/b; }",
     args=(1.0, 8.0), want=0.125, kind="dbl")
case("double mixed args", "double f(int n, double x){ return n*x; }",
     args=(3, 1.5), want=4.5, kind="dbl")
case("double compare", "int f(double x){ return x > 1.5; }", args=(2.0,), want=1)
case("double compare2", "int f(double x){ return x > 1.5; }", args=(1.0,), want=0)
case("double equality", "int f(){ return 0.1+0.2 == 0.3; }", want=0)
case("double loop",
     "double f(int n){ double s=0; int i; for(i=1;i<=n;i++) s += 1.0/i; return s; }",
     args=(10,), want=sum(1.0/i for i in range(1, 11)), kind="dbl")
case("double local array",
     "double f(){ double a[3]; a[0]=1.5; a[1]=2.5; a[2]=3.0; "
     "return a[0]+a[1]+a[2]; }", want=7.0, kind="dbl")
case("double global", "double g = 2.5; double f(){ return g*2; }", want=5.0,
     kind="dbl")
case("double negate", "double f(double x){ return -x; }", args=(2.5,), want=-2.5,
     kind="dbl")
case("double compound", "double f(){ double x=1.0; x+=0.5; x*=4; return x; }",
     want=6.0, kind="dbl")
case("double increment", "double f(){ double x=1.5; x++; return x; }", want=2.5,
     kind="dbl")
case("double to int round", "int f(){ return (int)(2.9999); }", want=2)
case("float storage",
     "float f2(){ return 1.5; } double f(){ float x = 2.5; return x*2; }",
     want=5.0, kind="dbl")
case("double struct",
     "struct V { double x; double y; }; "
     "double f(){ struct V v; v.x=3.0; v.y=4.0; return v.x*v.x+v.y*v.y; }",
     want=25.0, kind="dbl")
case("double recursion",
     "double pw(double b,int n){ return n==0?1.0:b*pw(b,n-1); } "
     "double f(){ return pw(2.0,10); }", want=1024.0, kind="dbl")

# ---------------------------------------------------------------- libm
case("sqrt", "double sqrt(double); double f(){ return sqrt(2.0); }",
     want=math.sqrt(2), kind="dbl")
case("sin", "double sin(double); double f(){ return sin(1.0); }",
     want=math.sin(1), kind="dbl")
case("pow", "double pow(double,double); double f(){ return pow(2.0,10.0); }",
     want=1024.0, kind="dbl")
case("nested libm",
     "double sin(double); double cos(double); double sqrt(double); "
     "double f(){ return sqrt(sin(1.0)*sin(1.0)+cos(1.0)*cos(1.0)); }",
     want=1.0, kind="dbl")

# ---------------------------------------------------------------- functions
case("many args",
     "int g(int a,int b,int c,int d,int e,int h){ return a+b*2+c*3+d*4+e*5+h*6; } "
     "int f(){ return g(1,1,1,1,1,1); }", want=21)
case("double many args",
     "double g(double a,double b,double c){ return a+b*2+c*3; } "
     "double f(){ return g(1.0,1.0,1.0); }", want=6.0, kind="dbl")
case("mixed many args",
     "double g(int a,double b,int c,double d){ return a+b+c+d; } "
     "double f(){ return g(1,2.5,3,4.25); }", want=10.75, kind="dbl")
case("forward call",
     "int later(int x); int f(){ return later(5); } int later(int x){ return x*3; }",
     want=15)
case("void function",
     "int g; void setg(int v){ g=v; } int f(){ setg(9); return g; }", want=9)

# ---------------------------------------------------------------- preprocessor
case("define", "#define N 42\nint f(){ return N; }", want=42)
case("define expr", "#define SQ(x) ((x)*(x))\nint f(){ return SQ(5); }", want=25)
case("define nested",
     "#define A 2\n#define B (A*3)\nint f(){ return B; }", want=6)
case("ifdef",
     "#define ON\n#ifdef ON\nint f(){ return 1; }\n#else\nint f(){ return 0; }\n#endif",
     want=1)
case("ifndef",
     "#ifndef OFF\nint f(){ return 7; }\n#else\nint f(){ return 0; }\n#endif",
     want=7)
case("if expr",
     "#define V 3\n#if V > 2\nint f(){ return 100; }\n#else\nint f(){ return 0; }\n#endif",
     want=100)
case("if false",
     "#if 0\nint f(){ return 0; }\n#else\nint f(){ return 55; }\n#endif", want=55)
case("macro in expr",
     "#define MAX 10\nint f(){ int i,s=0; for(i=0;i<MAX;i++) s+=i; return s; }",
     want=45)
case("comment handling",
     "int f(){ /* block\ncomment */ return 5; // line comment\n}", want=5)

# ---------------------------------------------------------------- printf
case("printf int", 'int printf(char*,...); int f(){ printf("n=%d", 42); return 0; }',
     want="n=42", kind="out")
case("printf double",
     'int printf(char*,...); int f(){ printf("%.3f", 3.14159); return 0; }',
     want="3.142", kind="out")
case("printf string",
     'int printf(char*,...); int f(){ printf("[%s]", "hi"); return 0; }',
     want="[hi]", kind="out")
case("printf mixed",
     'int printf(char*,...); int f(){ printf("%d %.1f %s", 7, 2.5, "x"); return 0; }',
     want="7 2.5 x", kind="out")
case("printf loop",
     'int printf(char*,...); int f(){ int i; for(i=0;i<3;i++) printf("%d,", i); return 0; }',
     want="0,1,2,", kind="out")

# ---------------------------------------------------------------- programs
case("bubble sort",
     "int f(){ int a[8]; int i,j,t; "
     "a[0]=5;a[1]=3;a[2]=8;a[3]=1;a[4]=9;a[5]=2;a[6]=7;a[7]=4; "
     "for(i=0;i<8;i++) for(j=0;j<7-i;j++) if(a[j]>a[j+1]){t=a[j];a[j]=a[j+1];a[j+1]=t;} "
     "return a[0]*100+a[7]; }", want=109)
case("primes",
     "int f(){ int n,i,c=0; for(n=2;n<100;n++){ int p=1; "
     "for(i=2;i*i<=n;i++) if(n%i==0){p=0;break;} c+=p; } return c; }", want=25)
case("gcd", "int gcd(int a,int b){ return b==0?a:gcd(b,a%b); } "
     "int f(){ return gcd(1071,462); }", want=21)
case("newton sqrt",
     "double f(){ double x=2.0, g=1.0; int i; "
     "for(i=0;i<20;i++) g = 0.5*(g + x/g); return g; }",
     want=math.sqrt(2), kind="dbl")
case("mandelbrot count",
     "int f(){ int cx,cy,count=0; for(cy=-12;cy<=12;cy++) for(cx=-20;cx<=8;cx++){ "
     "double zr=0, zi=0, pr=cx/10.0, pi=cy/10.0; int i; "
     "for(i=0;i<50;i++){ double t=zr*zr-zi*zi+pr; zi=2*zr*zi+pi; zr=t; "
     "if(zr*zr+zi*zi>4.0) break; } if(i==50) count++; } return count; }",
     want=None, kind="int_any")
case("string length",
     'int f(){ char *s="hello world"; int n=0; while(s[n]) n++; return n; }',
     want=11)
case("fizzbuzz sum",
     "int f(){ int i,s=0; for(i=1;i<=100;i++){ if(i%15==0) s+=15; "
     "else if(i%3==0) s+=3; else if(i%5==0) s+=5; } return s; }",
     want=sum(15 if i % 15 == 0 else 3 if i % 3 == 0 else 5 if i % 5 == 0 else 0
              for i in range(1, 101)))


# ------------------------------------------- argument conversion (prototypes)
case("int arg to double param",
     "double g(double x){ return x/4; } double f(){ return g(10); }",
     want=2.5, kind="dbl")
case("double arg to int param",
     "int g(int n){ return n*2; } int f(){ return g(3.9); }", want=6)
case("mixed proto conversion",
     "double g(double a,int b,double c){ return a+b+c; } "
     "double f(){ return g(1, 2, 3); }", want=6.0, kind="dbl")
case("libm int arg",
     "double sqrt(double); double f(){ return sqrt(16); }", want=4.0, kind="dbl")
case("libm int args pow",
     "double pow(double,double); double f(){ return pow(2,10); }",
     want=1024.0, kind="dbl")
case("forward proto conversion",
     "double later(double x); double f(){ return later(9); } "
     "double later(double x){ return x/2; }", want=4.5, kind="dbl")
case("printf double from int expr",
     'int printf(char*,...); double half(double x){ return x/2; } '
     'int f(){ printf("%.2f", half(5)); return 0; }', want="2.50", kind="out")
case("char param narrowing",
     "int g(char c){ return c; } int f(){ return g(321); }", want=65)


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    fails, ran = [], 0
    for label, src, fn, args, want, kind in CASES:
        if only and only not in label:
            continue
        ran += 1
        try:
            m = run_source(src)
            if kind == "out":
                m.call(fn, args)
                got = m.text()
                ok = (got == want)
            elif kind == "dbl":
                got = m.call(fn, args, want_double=True)
                ok = abs(got - want) <= 1e-12 * max(1.0, abs(want))
            elif kind == "int_any":
                got = m.call(fn, args)
                ok = isinstance(got, int)
            else:
                got = m.call(fn, args)
                ok = (got == want)
        except Exception as e:
            got = "%s: %s" % (type(e).__name__, e)
            ok = False
        if not ok:
            fails.append((label, want, got))
    for label, want, got in fails:
        print("FAIL  %-24s want %-22r got %r" % (label, want, got))
    print("\n%d/%d compiler tests passed" % (ran - len(fails), ran))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
