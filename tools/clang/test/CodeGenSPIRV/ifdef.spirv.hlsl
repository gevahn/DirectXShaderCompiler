// RUN: %dxc -T ps_6_0 -E main

#ifndef __spirv
[[vk::constant_id(1)]] const float foo = 0.3;
#endif

#ifdef __spirv
[[vk::constant_id(1)]] const float bar = 0.3;
#endif

float4 main(float4 color : COLOR) : SV_TARGET
{
// CHECK: error: use of undeclared identifier 'foo'
    color.x = foo;

// CHECK-NOT: error: use of undeclared identifier 'bar'
// CHECK:     error: use of undeclared identifier 'zoo'
#ifdef __spirv
    color.y = bar;
    color.z = zoo;
#endif
    return color;
}
