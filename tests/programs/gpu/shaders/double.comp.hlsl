// The compute shader of tests/programs/gpu: each number of the buffer doubled, plus the value
// of a uniform. SDL_GPU: read-write storage buffers in space1, uniform buffers in space2.

RWStructuredBuffer<uint> numbers : register(u0, space1);

cbuffer Extra : register(b0, space2)
{
    uint extra;
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    numbers[id.x] = numbers[id.x] * 2 + extra;
}
