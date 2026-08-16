#include "prop.c"

int main(void)
{
    fe_writer w;
    unsigned short result;
    w.tag=2;
    w.handle=99;
    result=fe_m4_prop_propagate(w);
    return result==1 ? 0 : 1;
}
