#pragma once

struct RayHit {
	float t;
	float u, v;

	int mesh_id;
	int triangle_id;
};

struct Ray {
	float3 origin;
	float3 direction;
	float3 direction_inv;

	__device__ inline void calc_direction_inv() {
		direction_inv = make_float3(
			1.0f / direction.x, 
			1.0f / direction.y, 
			1.0f / direction.z
		);
	}
};
