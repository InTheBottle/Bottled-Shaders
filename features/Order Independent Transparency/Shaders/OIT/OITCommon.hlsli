#ifndef __OIT_COMMON__
#define __OIT_COMMON__

#ifndef OIT_NODE_COUNT
#	define OIT_NODE_COUNT 8
#endif

#if OIT_NODE_COUNT >= 8
#	define AOIT_DONT_COMPRESS_FIRST_HALF
#endif

#define OIT_TILED_ADDRESSING 1

#if OIT_NODE_COUNT == 2
#	define OIT_RT_COUNT 1
#else
#	define OIT_RT_COUNT (OIT_NODE_COUNT / 4)
#endif

static const uint OIT_FEATURE_FLAGS_SHIFT = 11;
static const uint OIT_FLAGS_ADDITIVE = 0x1;
static const uint OIT_FLAGS_MULTIPLICATIVE = 0x2;
static const uint OIT_FLAGS_MULTIPLICATIVE_A = 0x3;
static const uint OIT_FLAGS_BLEND_MODES = 0x3;
static const uint OIT_FLAGS_DEPTH_WRITE = 0x4;
static const uint OIT_FLAGS_DISABLED = 0x8;
static const float OIT_EMPTY_NODE_DEPTH = 3.40282E38f;
static const float OIT_MIN_CONTRIBUTION = 0.5 / 255.0;
static const float OIT_DEPTH_EPSILON = 1.0 / 8388608.0;

float OIT_Luminance(float3 color)
{
	return dot(max(color, 0.0), float3(0.2126, 0.7152, 0.0722));
}

float OIT_FarDepth()
{
#ifdef REVERSE_Z
	return 0.0;
#else
	return 1.0;
#endif
}

float OIT_Nearness(float depth)
{
#ifdef REVERSE_Z
	return depth;
#else
	return 1.0 - depth;
#endif
}

float OIT_SortKey(float depth)
{
	return -OIT_Nearness(depth);
}

bool OIT_IsNearerOrEqual(float depth, float referenceDepth)
{
#ifdef REVERSE_Z
	return depth >= referenceDepth - OIT_DEPTH_EPSILON;
#else
	return depth <= referenceDepth + OIT_DEPTH_EPSILON;
#endif
}

#endif
