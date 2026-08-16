#include "prop.c"

static unsigned short calls;

static unsigned short fail_write(void *ctx, const unsigned char *p,
                                  unsigned long n)
{
    (void)ctx;
    (void)p;
    (void)n;
    ++calls;
    return calls == 1 ? 7 : 0;
}

int main(void)
{
    fe_writer w;
    unsigned short result;
    w.ctx=0;
    w.write_fn=fail_write;
    result=fe_m4_prop_propagate(&w);
    return (result==7 && calls==1) ? 0 : 1;
}
