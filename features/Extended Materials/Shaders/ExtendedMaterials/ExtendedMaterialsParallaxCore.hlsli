#ifndef EXTENDED_MATERIALS_PARALLAX_CORE_HLSLI
#define EXTENDED_MATERIALS_PARALLAX_CORE_HLSLI

#if defined(LANDSCAPE)
	float2 GetParallaxCoords(PS_INPUT input, float2 coords, float mipLevel, float maxTexDim, float3 viewDir, float3x3 tbn, float noise, DisplacementParams params[TERRAIN_LAYER_COUNT],
		StochasticOffsets sharedOffset,
		out float weights[TERRAIN_LAYER_COUNT])
#else
	float2 GetParallaxCoords(float2 coords, float mipLevel, float3 viewDir, float3x3 tbn, Texture2D<float4> tex, SamplerState texSampler, uint channel, DisplacementParams params, bool applyMeshTV, StochasticOffsets meshOffset)
#endif
	{
		float3 viewDirTS = normalize(mul(tbn, viewDir));
		float ndotv = saturate(viewDirTS.z);
		float softZ = SharedData::extendedMaterialSettings.EnableParallaxWarpingFix ? abs(viewDirTS.z) * 0.7 + 0.3 : max(abs(viewDirTS.z), 0.15);
#if defined(LANDSCAPE)
		float parallaxZ = max(softZ + params[0].FlattenAmount, 0.0625);
#else
		float parallaxZ = max(softZ + params.FlattenAmount, 0.0625);
#endif
		float2 parallaxDir = viewDirTS.xy / parallaxZ;

#if defined(LANDSCAPE)
		float4 w1 = input.LandBlendWeights1;
		float2 w2 = input.LandBlendWeights2.xy;
		const float marchHeightBlendFactor = 0.0;

		weights[0] = w1.x;
		weights[1] = w1.y;
		weights[2] = w1.z;
		weights[3] = w1.w;
		weights[4] = w2.x;
		weights[5] = w2.y;
#	if defined(LANDSCAPE_SEAMS)
		weights[6] = LandscapeSeams::ExtraWeights.x;
		weights[7] = LandscapeSeams::ExtraWeights.y;
		weights[8] = LandscapeSeams::ExtraWeights.z;
		weights[9] = LandscapeSeams::ExtraWeights.w;
#	endif

#	if defined(TRUE_PBR)
		float scale = TerrainMaxWeightedHeightScaleW(w1, w2, params);
		float terrainHeightNormMul = rcp(max(scale, 1e-4));
		float maxHeight = 0.1 * scale;
#	else
		float scale = 1;
		float terrainHeightNormMul = 1.0;
		float maxHeight = 0.1 * scale;
#	endif
#else
		float scale = params.HeightScale;
		float maxHeight = 0.1 * scale;
#endif
		float minHeight = maxHeight * 0.5;
		float2 resultCoords = coords;

#if defined(LANDSCAPE) && defined(TRUE_PBR)
		[branch] if (scale <= 0.001) {
			if (SharedData::extendedMaterialSettings.EnableHeightBlending) {
				float unusedHeight = GetTerrainHeight(coords, mipLevel, params, 1.0, w1, w2, sharedOffset, weights);
			}
		} else
#elif !defined(LANDSCAPE)
		[branch] if (scale > 0.001)
#endif
		{
			const float density = ParallaxDensity();
			const uint minSteps = 4;
#if defined(LANDSCAPE)
			const uint maxStepsCap = clamp((uint)(40.0 * density + 0.5), 8u, 128u);
#else
			const uint maxStepsCap = clamp((uint)(32.0 * density + 0.5), 8u, 128u);
			const float baseMaxSteps = 8;
#endif

			// Squared grazing factor; near head-on stays cheap.
			float grazing = (1.0 - ndotv);
			grazing *= grazing;

			// Step count from UV travel in texels (and a grazing angle floor), so grazing rays do not skip height features between samples.
			// Texels are counted at the mip being sampled, so the march thins out with distance
			// because the heightfield genuinely holds fewer texels there.
#if !defined(LANDSCAPE)
			float2 texDims;
			tex.GetDimensions(texDims.x, texDims.y);
			float maxTexDim = max(texDims.x, texDims.y);
#endif
			float mipTexDim = maxTexDim * exp2(-mipLevel);
			float uvMarchSpan = dot(abs(parallaxDir), maxHeight + minHeight);
			float marchTexels = uvMarchSpan * mipTexDim;
#if defined(LANDSCAPE)
			float texelsPerStep = lerp(3.5, 1.75, grazing) * rcp(density);
			uint angleSteps = (uint)(lerp((float)minSteps, (float)maxStepsCap, grazing) + 0.5);
#else
			float texelsPerStep = lerp(7.0, 3.5, grazing) * rcp(density);
			float grazingStepBoost = lerp(1.0, 1.65, grazing);
			float angleStepMul = clamp(0.5 * rcp(max(ndotv, 0.0625)), 0.5, 2.5);
			uint angleSteps = (uint)(scale * baseMaxSteps * angleStepMul * grazingStepBoost * density);
#endif
			uint uvSteps = (uint)(marchTexels * rcp(texelsPerStep) + 0.5);
			// Past one step per texel the extra taps land in a texel already read.
			uint numSteps = min(max(uvSteps, angleSteps), (uint)(marchTexels + 0.5));
			numSteps = clamp(numSteps, minSteps, maxStepsCap);
			numSteps = (numSteps + 2) & ~3;

			uint contactIters = grazing > 0.2 ? 2u : 1u;

			float stepSize = rcp((float)numSteps);

			float2 offsetPerStep = parallaxDir * maxHeight * stepSize;
#if defined(LANDSCAPE)
			// Full-step ray-start dither breaks residual step bands on terrain.
			float rayDither = saturate(noise);
			float2 prevOffset = parallaxDir * minHeight + coords.xy + offsetPerStep * rayDither;
			float prevBound = 1.0 + rayDither * stepSize;
#else
			float2 prevOffset = parallaxDir * minHeight + coords.xy;
			float prevBound = 1.0;
#endif
			float prevHeight = 1.0;

			float2 pt1 = 0;
			float2 pt2 = 0;
			bool intersectionFound = false;

			[loop] while (numSteps > 0)
			{
				float4 currentOffset[2];
				currentOffset[0] = prevOffset.xyxy - float4(1, 1, 2, 2) * offsetPerStep.xyxy;
				currentOffset[1] = prevOffset.xyxy - float4(3, 3, 4, 4) * offsetPerStep.xyxy;
				float4 currentBound = prevBound.xxxx - float4(1, 2, 3, 4) * stepSize;

				float4 currHeight;
#if defined(LANDSCAPE)
				currHeight = GetTerrainHeightQuadRayMarch(currentOffset[0].xy, currentOffset[0].zw, currentOffset[1].xy, currentOffset[1].zw, mipLevel, params, marchHeightBlendFactor, w1, w2, sharedOffset, weights) * terrainHeightNormMul + 0.5;
#else
#	if defined(TERRAIN_VARIATION)
				[branch] if (applyMeshTV)
				{
					currHeight.x = StochasticHeightChannel(tex, texSampler, currentOffset[0].xy, mipLevel, channel, meshOffset);
					currHeight.y = StochasticHeightChannel(tex, texSampler, currentOffset[0].zw, mipLevel, channel, meshOffset);
					currHeight.z = StochasticHeightChannel(tex, texSampler, currentOffset[1].xy, mipLevel, channel, meshOffset);
					currHeight.w = StochasticHeightChannel(tex, texSampler, currentOffset[1].zw, mipLevel, channel, meshOffset);
				}
				else
#	endif
				{
					currHeight.x = tex.SampleLevel(texSampler, currentOffset[0].xy, mipLevel)[channel];
					currHeight.y = tex.SampleLevel(texSampler, currentOffset[0].zw, mipLevel)[channel];
					currHeight.z = tex.SampleLevel(texSampler, currentOffset[1].xy, mipLevel)[channel];
					currHeight.w = tex.SampleLevel(texSampler, currentOffset[1].zw, mipLevel)[channel];
				}

				currHeight = AdjustDisplacementNormalized(currHeight, params);
#endif

				bool4 testResult = currHeight >= currentBound;
				[branch] if (any(testResult))
				{
					intersectionFound = true;
					[branch] if (testResult.x)
					{
						pt1 = float2(currentBound.x, currHeight.x);
						pt2 = float2(prevBound, prevHeight);
					}
					else if (testResult.y)
					{
						pt1 = float2(currentBound.y, currHeight.y);
						pt2 = float2(currentBound.x, currHeight.x);
					}
					else if (testResult.z)
					{
						pt1 = float2(currentBound.z, currHeight.z);
						pt2 = float2(currentBound.y, currHeight.y);
					}
					else
					{
						pt1 = float2(currentBound.w, currHeight.w);
						pt2 = float2(currentBound.z, currHeight.z);
					}
					break;
				}

				prevOffset = currentOffset[1].zw;
				prevBound = currentBound.w;
				prevHeight = currHeight.w;
				numSteps -= 4;
			}

			float parallaxAmount = 0.0;
			[branch] if (intersectionFound)
			{
				float tNear = pt1.x;
				float fNear = pt1.y - tNear;
				float tFar = pt2.x;
				float fFar = pt2.y - tFar;

				// Binary search on f(t) = h(t) - t before secant.
				[loop] for (uint c = 0; c < contactIters; c++)
				{
					float tMid = 0.5 * (tNear + tFar);
					float2 midCoords = coords.xy + parallaxDir * (((1.0 - tMid) * -maxHeight) + minHeight);
					float hMid;
#if defined(LANDSCAPE)
					hMid = GetTerrainHeight(midCoords, mipLevel, params, marchHeightBlendFactor, w1, w2, sharedOffset, weights) * terrainHeightNormMul + 0.5;
#else
					hMid = tex.SampleLevel(texSampler, midCoords, mipLevel)[channel];
					hMid = AdjustDisplacementNormalized(hMid, params);
#endif
					float fMid = hMid - tMid;
					[branch] if (fMid >= 0.0)
					{
						tNear = tMid;
						fNear = fMid;
					}
					else
					{
						tFar = tMid;
						fFar = fMid;
					}
				}

				// Secant step on f(t) = h(t) - t.
				{
					float denominator = fNear - fFar;
					float r = abs(denominator) > EPSILON_DIVISION ? saturate(fNear / denominator) : 0.5;
					float tSecant = lerp(tNear, tFar, r);
					float2 secantCoords = coords.xy + parallaxDir * (((1.0 - tSecant) * -maxHeight) + minHeight);

					float hSecant;
#if defined(LANDSCAPE)
					hSecant = GetTerrainHeight(secantCoords, mipLevel, params, marchHeightBlendFactor, w1, w2, sharedOffset, weights) * terrainHeightNormMul + 0.5;
#else
					hSecant = tex.SampleLevel(texSampler, secantCoords, mipLevel)[channel];
					hSecant = AdjustDisplacementNormalized(hSecant, params);
#endif

					float fSecant = hSecant - tSecant;
					[branch] if (fSecant >= 0.0)
					{
						tNear = tSecant;
						fNear = fSecant;
					}
					else
					{
						tFar = tSecant;
						fFar = fSecant;
					}
				}

				float denominator = fNear - fFar;
				float r = abs(denominator) > EPSILON_DIVISION ? saturate(fNear / denominator) : 0.5;
				parallaxAmount = lerp(tNear, tFar, r);
			}

			float offset = (1.0 - parallaxAmount) * -maxHeight + minHeight;
			float2 finalCoords = parallaxDir * offset + coords.xy;
#if defined(LANDSCAPE)
			if (SharedData::extendedMaterialSettings.EnableHeightBlending) {
				float unusedHeight = GetTerrainHeight(finalCoords, mipLevel, params, 1.0, w1, w2, sharedOffset, weights);
			}
#endif
			resultCoords = finalCoords;
		}

		return resultCoords;
	}

#	if !defined(LANDSCAPE)
	float GetParallaxSoftShadowMultiplier(float2 coords, float mipLevel, float3 L, float sh0, Texture2D<float4> tex, SamplerState texSampler, uint channel, float quality, float noise, DisplacementParams params, bool applyMeshTV, StochasticOffsets meshOffset, uint maxTaps)
	{
		float shadow = 1.0;
		sh0 = AdjustDisplacementNormalized(sh0, params);
		float rise = AdjustDisplacementNormalized(1.0, params) - sh0;
		float horizon = saturate(L.z * rcp(ParallaxShadowMinLightZ));
		[branch] if (quality > 0.0 && rise > 0.0 && horizon > 0.0)
		{
			uint taps = ParallaxShadowTaps(quality, maxTaps);
			float invTaps = rcp((float)taps);
			float2 rayUV = L.xy * rcp(max(L.z, ParallaxShadowMinLightZ)) * (0.1 * params.HeightScale * rise);
			float occlusion = 0.0;
			[loop] for (uint i = 0; i < taps; i += 4)
			{
				bool4 valid = (uint4(0, 1, 2, 3) + i) < taps;
				float4 t = (float4(0, 1, 2, 3) + (float)i + noise) * invTaps;
				float4 rayHeight = sh0 + rise * t;
				float4 h = 0.0;
#		if defined(TERRAIN_VARIATION)
				[branch] if (applyMeshTV)
				{
					h.x = StochasticHeightChannel(tex, texSampler, coords + rayUV * t.x, mipLevel, channel, meshOffset);
					if (valid.y)
						h.y = StochasticHeightChannel(tex, texSampler, coords + rayUV * t.y, mipLevel, channel, meshOffset);
					if (valid.z)
						h.z = StochasticHeightChannel(tex, texSampler, coords + rayUV * t.z, mipLevel, channel, meshOffset);
					if (valid.w)
						h.w = StochasticHeightChannel(tex, texSampler, coords + rayUV * t.w, mipLevel, channel, meshOffset);
				}
				else
#		endif
				{
					h.x = tex.SampleLevel(texSampler, coords + rayUV * t.x, mipLevel)[channel];
					if (valid.y)
						h.y = tex.SampleLevel(texSampler, coords + rayUV * t.y, mipLevel)[channel];
					if (valid.z)
						h.z = tex.SampleLevel(texSampler, coords + rayUV * t.z, mipLevel)[channel];
					if (valid.w)
						h.w = tex.SampleLevel(texSampler, coords + rayUV * t.w, mipLevel)[channel];
				}
				h = AdjustDisplacementNormalized(h, params);
				float4 blocked = max(0.0, h - rayHeight - ParallaxShadowBias) * (1.0 - t);
				blocked = valid ? blocked : 0.0;
				occlusion = max(occlusion, max(max(blocked.x, blocked.y), max(blocked.z, blocked.w)));
			}
			shadow = 1.0 - saturate(occlusion * ShadowIntensity * 4.0) * horizon;
		}
		return shadow;
	}

#	endif

#endif  // EXTENDED_MATERIALS_PARALLAX_CORE_HLSLI
