extern long fe_m5_runtime_run(long mode);

int main(void)
{
    if (fe_m5_runtime_run(0) != 0) return 1;
    if (fe_m5_runtime_run(1) != 9) return 2;
    if (fe_m5_runtime_run(2) != 0) return 3;
    return 0;
}
