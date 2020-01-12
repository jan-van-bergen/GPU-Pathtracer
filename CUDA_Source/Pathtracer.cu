#include "vector_types.h"
#include "cuda_math.h"

#include <corecrt_math.h>

#include "../Common.h"

#define ASSERT(proposition) assert(proposition)
//#define ASSERT(proposition) 

surface<void, 2> frame_buffer;

#define USE_IMPORTANCE_SAMPLING true

// Based on: http://www.reedbeta.com/blog/quick-and-easy-gpu-random-numbers-in-d3d11/
__device__ unsigned wang_hash(unsigned seed) {
	seed = (seed ^ 61) ^ (seed >> 16);
	seed = seed + (seed << 3);
	seed = seed ^ (seed >> 4);
	seed = seed * 0x27d4eb2d;
	seed = seed ^ (seed >> 15);

	return seed;
}

// Based on: http://www.reedbeta.com/blog/quick-and-easy-gpu-random-numbers-in-d3d11/
__device__ unsigned rand_xorshift(unsigned & seed) {
    seed ^= (seed << 13);
    seed ^= (seed >> 17);
	seed ^= (seed << 5);
	
    return seed;
}

__device__ float random_float(unsigned & seed) {
	const float one_over_max_unsigned = 2.3283064365387e-10f;
	return float(rand_xorshift(seed)) * one_over_max_unsigned;
}

struct Material;

__device__ Material            * materials;
__device__ cudaTextureObject_t * textures;

struct Material {
	float3 diffuse;
	int texture_id;

	float3 emittance;

	__device__ float3 albedo(float u, float v) const {
		if (texture_id == -1) return diffuse;

		float4 tex_colour;

		for (int i = 0; i < MAX_TEXTURES; i++) {
			if (texture_id == i) {
				tex_colour = tex2D<float4>(textures[i], u, v);
			}
		}

		return diffuse * make_float3(tex_colour);
	}
};

__device__ int     light_count;
__device__ int   * light_indices;
__device__ float * light_areas;
__device__ float total_light_area;

struct Ray {
	float3 origin;
	float3 direction;
	float3 direction_inv;
};

struct RayHit {
	float distance = INFINITY;
	
	int material_id;

	float3 point;
	float3 normal;
	float2 uv;
};

struct AABB {
	float3 min;
	float3 max;

	__device__ inline bool intersects(const Ray & ray, float max_distance) const {
		float3 t0 = (min - ray.origin) * ray.direction_inv;
		float3 t1 = (max - ray.origin) * ray.direction_inv;
		
		float3 t_min = fminf(t0, t1);
		float3 t_max = fmaxf(t0, t1);
		
		float t_near = fmaxf(fmaxf(EPSILON,      t_min.x), fmaxf(t_min.y, t_min.z));
		float t_far  = fminf(fminf(max_distance, t_max.x), fminf(t_max.y, t_max.z));
	
		return t_near < t_far;
	}
};

struct Triangle {
	AABB aabb;

	float3 position0;
	float3 position1;
	float3 position2;

	float3 normal0;
	float3 normal1;
	float3 normal2; 
	
	float2 tex_coord0;
	float2 tex_coord1;
	float2 tex_coord2;

	int material_id;
};

__device__ Triangle * triangles;

struct BVHNode {
	AABB aabb;
	union {
		int left;
		int first;
	};
	int count;

	__device__ inline bool is_leaf() const {
		return (count & (~BVH_AXIS_MASK)) > 0;
	}

	__device__ inline bool should_visit_left_first(const Ray & ray) const {
#if BVH_TRAVERSAL_STRATEGY == BVH_TRAVERSE_TREE_NAIVE
		return true; // Naive always goes left first
#elif BVH_TRAVERSAL_STRATEGY == BVH_TRAVERSE_TREE_ORDERED
		switch (count & BVH_AXIS_MASK) {
			case BVH_AXIS_X_BITS: return ray.direction.x > 0.0f;
			case BVH_AXIS_Y_BITS: return ray.direction.y > 0.0f;
			case BVH_AXIS_Z_BITS: return ray.direction.z > 0.0f;
		}
#endif
	}
};

__device__ BVHNode * bvh_nodes;

__device__ float3 camera_position;
__device__ float3 camera_top_left_corner;
__device__ float3 camera_x_axis;
__device__ float3 camera_y_axis;

__device__ void triangle_trace(const Triangle & triangle, const Ray & ray, RayHit & ray_hit) {
	float3 edge1 = triangle.position1 - triangle.position0;
	float3 edge2 = triangle.position2 - triangle.position0;

	float3 h = cross(ray.direction, edge2);
	float  a = dot(edge1, h);

	float  f = 1.0f / a;
	float3 s = ray.origin - triangle.position0;
	float  u = f * dot(s, h);

	if (u < 0.0f || u > 1.0f) return;

	float3 q = cross(s, edge1);
	float  v = f * dot(ray.direction, q);

	if (v < 0.0f || u + v > 1.0f) return;

	float t = f * dot(edge2, q);

	if (t < EPSILON || t >= ray_hit.distance) return;

	ray_hit.distance = t;

	ray_hit.material_id = triangle.material_id;

	ray_hit.point = ray.origin + t * ray.direction;
	ray_hit.normal = normalize(triangle.normal0 
		+ u * (triangle.normal1 - triangle.normal0) 
		+ v * (triangle.normal2 - triangle.normal0)
	);
	ray_hit.uv = triangle.tex_coord0 
		+ u * (triangle.tex_coord1 - triangle.tex_coord0) 
		+ v * (triangle.tex_coord2 - triangle.tex_coord0);
}

__device__ bool triangle_intersect(const Triangle & triangle, const Ray & ray, float max_distance) {
	float3 edge1 = triangle.position1 - triangle.position0;
	float3 edge2 = triangle.position2 - triangle.position0;

	float3 h = cross(ray.direction, edge2);
	float  a = dot(edge1, h);

	float  f = 1.0f / a;
	float3 s = ray.origin - triangle.position0;
	float  u = f * dot(s, h);

	if (u < 0.0f || u > 1.0f) return false;

	float3 q = cross(s, edge1);
	float  v = f * dot(ray.direction, q);

	if (v < 0.0f || u + v > 1.0f) return false;

	float t = f * dot(edge2, q);

	if (t < EPSILON || t >= max_distance) return false;

	return true;
}

__device__ void bvh_trace(const Ray & ray, RayHit & ray_hit) {
	int stack[64];
	int stack_size = 1;

	// Push root on stack
	stack[0] = 0;

	while (stack_size > 0) {
		// Pop Node of the stack
		const BVHNode & node = bvh_nodes[stack[--stack_size]];

		if (node.aabb.intersects(ray, ray_hit.distance)) {
			if (node.is_leaf()) {
				for (int i = node.first; i < node.first + node.count; i++) {
					triangle_trace(triangles[i], ray, ray_hit);
				}
			} else {
				if (node.should_visit_left_first(ray)) {
					stack[stack_size++] = node.left + 1;
					stack[stack_size++] = node.left;
				} else {
					stack[stack_size++] = node.left;
					stack[stack_size++] = node.left + 1;
				}
			}
		}
	}
}

__device__ bool bvh_intersect(const Ray & ray, float max_distance) {
	int stack[64];
	int stack_size = 1;

	// Push root on stack
	stack[0] = 0;

	while (stack_size > 0) {
		// Pop Node of the stack
		const BVHNode & node = bvh_nodes[stack[--stack_size]];

		if (node.aabb.intersects(ray, max_distance)) {
			if (node.is_leaf()) {
				for (int i = node.first; i < node.first + node.count; i++) {
					if (triangle_intersect(triangles[i], ray, max_distance)) {
						return true;
					}
				}
			} else {
				if (node.should_visit_left_first(ray)) {
					stack[stack_size++] = node.left + 1;
					stack[stack_size++] = node.left;
				} else {
					stack[stack_size++] = node.left;
					stack[stack_size++] = node.left + 1;
				}
			}
		}
	}

	return false;
}

__device__ int      sky_size;
__device__ float3 * sky_data;

__device__ float3 sample_sky(const float3 & direction) {
	// Formulas as described on https://www.pauldebevec.com/Probes/
    float r = 0.5f * ONE_OVER_PI * acos(direction.z) * rsqrt(direction.x*direction.x + direction.y*direction.y);

	float u = direction.x * r + 0.5f;
	float v = direction.y * r + 0.5f;

	// Convert to pixel coordinates
	int x = int(u * sky_size);
	int y = int(v * sky_size);

	int index = x + y * sky_size;
	index = max(index, 0);
	index = min(index, sky_size * sky_size);

	return sky_data[index];
}

__device__ float3 diffuse_reflection(unsigned & seed, const float3 & normal) {
	float3 direction;
	float  length_squared;

	// Find a random point inside the unit sphere
	do {
		direction.x = -1.0f + 2.0f * random_float(seed);
		direction.y = -1.0f + 2.0f * random_float(seed);
		direction.z = -1.0f + 2.0f * random_float(seed);

		length_squared = dot(direction, direction);
	} while (length_squared > 1.0f);

	// Normalize direction to obtain a random point on the unit sphere
	float  inv_length = rsqrt(length_squared);
	float3 random_point_on_unit_sphere = inv_length * direction;

	// If the point is on the wrong hemisphere, return its negative
	if (dot(normal, random_point_on_unit_sphere) < 0.0f) {
		return -random_point_on_unit_sphere;
	}

	return random_point_on_unit_sphere;
}

__device__ float3 cosine_weighted_diffuse_reflection(unsigned & seed, const float3 & normal) {
	float r0 = random_float(seed);
	float r1 = random_float(seed);

	float sin_theta, cos_theta;
	sincos(TWO_PI * r1, &sin_theta, &cos_theta);

	float r = sqrtf(r0);
	float x = r * cos_theta;
	float y = r * sin_theta;
	
	float3 direction = normalize(make_float3(x, y, sqrtf(1.0f - r0)));
	
	// Calculate a tangent vector from the normal vector
	float3 tangent;
	if (fabs(normal.x) > fabs(normal.y)) {
		tangent = make_float3(normal.z, 0.0f, -normal.x) * rsqrt(normal.x * normal.x + normal.z * normal.z);
	} else {
		tangent = make_float3(0.0f, -normal.z, normal.y) * rsqrt(normal.y * normal.y + normal.z * normal.z);
	}

	// The binormal is perpendicular to both the normal and tangent vectors
	float3 binormal = cross(normal, tangent);

	// Multiply the direction with the TBN matrix
	return normalize(make_float3(
		tangent.x * direction.x + binormal.x * direction.y + normal.x * direction.z, 
		tangent.y * direction.x + binormal.y * direction.y + normal.y * direction.z, 
		tangent.z * direction.x + binormal.z * direction.y + normal.z * direction.z
	));
}

__device__ int random_light(unsigned & seed) {
	// return light_indices[rand_xorshift(seed) % light_count];
	float p = random_float(seed) * total_light_area;

	// int first = 0;
	// int last  = light_count - 1;
	// int middle = (first + last) >> 1;

	// while (first <= last) {
	// 	if (middle == 0 || (light_areas[middle] >= p && light_areas[middle - 1] < p)) {
	// 		break;
	// 	}

	// 	if (light_areas[middle] < p) {
	// 		first = middle + 1;
	// 	} else {
	// 		last = middle - 1;
	// 	}

	// 	middle = (first + last) >> 1;
	// }

	int middle = 0;
	while (light_areas[middle] < p) {
		middle++;

		ASSERT(middle < light_count);
	}

	return light_indices[middle];
}

__device__ float3 sample(unsigned & seed, Ray & ray) {
	const int ITERATIONS = 5;
	
	float3 colour     = make_float3(0.0f);
	float3 throughput = make_float3(1.0f);
	
	for (int bounce = 0; bounce < ITERATIONS; bounce++) {
		// Check ray against all triangles
		RayHit hit;
		bvh_trace(ray, hit);

		// Check if we didn't hit anything
		if (hit.distance == INFINITY) {
			return throughput * sample_sky(ray.direction);
		}

		const Material & material = materials[hit.material_id];

		// Check if we hit a Light
		if (dot(material.emittance, material.emittance) > 0.0f) {
			return throughput * material.emittance;
		}

		// Create new Ray in random direction on the hemisphere defined by the normal
#if USE_IMPORTANCE_SAMPLING
		float3 direction = cosine_weighted_diffuse_reflection(seed, hit.normal);
#else
		float3 direction = diffuse_reflection(seed, hit.normal);
#endif	

		if (light_count == 0) {
#if USE_IMPORTANCE_SAMPLING
			throughput *= material.albedo(hit.uv.x, hit.uv.y);
#else
			throughput *= 2.0f * material.albedo(hit.uv.x, hit.uv.y) * dot(hit.normal, direction);
#endif
		} else {
			const Triangle & light_triangle = triangles[random_light(seed)];

			ASSERT(length(materials[light_triangle.material_id].emittance) > 0.0f);
		
			// Pick a random point on the triangle using random barycentric coordinates
			float u = random_float(seed);
			float v = random_float(seed);

			if (u + v > 1.0f) {
				u = 1.0f - u;
				v = 1.0f - v;
			}

			float3 edge1 = light_triangle.position1 - light_triangle.position0;
			float3 edge2 = light_triangle.position2 - light_triangle.position0;

			float3 random_point_on_light = light_triangle.position0 + u * edge1 + v * edge2;

			// Calculate the area of the triangle light
			float light_area = 0.5f * length(cross(edge1, edge2));

			float3 to_light = random_point_on_light - hit.point;
			float distance_to_light_squared = dot(to_light, to_light);
			float distance_to_light         = sqrtf(distance_to_light_squared);

			// Normalize the vector to the light
			to_light /= distance_to_light;

			float3 light_normal = light_triangle.normal0 
				+ u * (light_triangle.normal1 - light_triangle.normal0)
				+ v * (light_triangle.normal2 - light_triangle.normal0);

			float cos_o = -dot(to_light, light_normal);
			float cos_i =  dot(to_light, hit.normal);

			if (cos_o <= 0.0f || cos_i <= 0.0f) break;

			ray.origin    = hit.point;
			ray.direction = to_light;
			ray.direction_inv = make_float3(
				1.0f / ray.direction.x, 
				1.0f / ray.direction.y, 
				1.0f / ray.direction.z
			);

			// Check if the light is obstructed by any other object in the scene
			if (bvh_intersect(ray, distance_to_light - EPSILON)) break;

			float3 brdf = material.albedo(hit.uv.x, hit.uv.y) * ONE_OVER_PI;
			float solid_angle = (cos_o * light_area) / distance_to_light_squared;

			float3 light_colour = materials[light_triangle.material_id].emittance;

			return brdf * light_count * light_colour * solid_angle * cos_i;
		}

		// Russian Roulette termination after at least four bounces
		if (bounce > 3) {
			float one_minus_p = fmaxf(throughput.x, fmaxf(throughput.y, throughput.z));
			if (random_float(seed) > one_minus_p) {
				break;
			}

			throughput /= one_minus_p;
		}
		
		ray.origin    = hit.point;
		ray.direction = direction;
		ray.direction_inv = make_float3(
			1.0f / ray.direction.x, 
			1.0f / ray.direction.y, 
			1.0f / ray.direction.z
		);
	}

	return make_float3(0.0f);
}

extern "C" __global__ void trace_ray(int random, float frames_since_camera_moved) {
	int x = blockIdx.x * blockDim.x + threadIdx.x;
	int y = blockIdx.y * blockDim.y + threadIdx.y;

	unsigned seed = wang_hash((x + y * SCREEN_WIDTH) * random);
	
	// Add random value between 0 and 1 so that after averaging we get anti-aliasing
	float u = x + random_float(seed);
	float v = y + random_float(seed);

	// Create primary Ray that starts at the Camera's position and goes trough the current pixel
	Ray ray;
	ray.origin    = camera_position;
	ray.direction = normalize(camera_top_left_corner
		+ u * camera_x_axis
		+ v * camera_y_axis
	);
	ray.direction_inv = make_float3(1.0f / ray.direction.x, 1.0f / ray.direction.y, 1.0f / ray.direction.z);
	
	float3 colour = sample(seed, ray);

	// If the Camera hasn't moved, average over previous frames
	if (frames_since_camera_moved > 0.0f) {
		float4 prev;
		surf2Dread<float4>(&prev, frame_buffer, x * sizeof(float4), y);

		// Take average over n samples by weighing the current content of the framebuffer by (n-1) and the new sample by 1
		colour = (make_float3(prev) * (frames_since_camera_moved - 1.0f) + colour) / frames_since_camera_moved;
	}

	surf2Dwrite<float4>(make_float4(colour, 1.0f), frame_buffer, x * sizeof(float4), y, cudaBoundaryModeClamp);
}