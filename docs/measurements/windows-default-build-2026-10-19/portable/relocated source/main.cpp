namespace { __declspec(noinline) int branch(int x) { if(x>4)return x-1; return x+3; } }
int main(){ volatile int x=7; int(*volatile fn)(int)=branch; for(int i=0;i<30;i++)x=fn(x); return 0; }
