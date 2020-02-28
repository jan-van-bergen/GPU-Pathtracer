#include "Pathtracer.h"

#include <filesystem>

#include "CUDAMemory.h"
#include "CUDAContext.h"

#include "MeshData.h"
#include "BVH.h"
#include "MBVH.h"

#include "Sky.h"

#include "BlueNoise.h"

#include "ScopedTimer.h"

static struct ExtendBuffer {
	CUDAMemory::Ptr<float> origin_x;
	CUDAMemory::Ptr<float> origin_y;
	CUDAMemory::Ptr<float> origin_z;
	CUDAMemory::Ptr<float> direction_x;
	CUDAMemory::Ptr<float> direction_y;
	CUDAMemory::Ptr<float> direction_z;
	
	CUDAMemory::Ptr<int>   pixel_index;
	CUDAMemory::Ptr<float> throughput_x;
	CUDAMemory::Ptr<float> throughput_y;
	CUDAMemory::Ptr<float> throughput_z;

	CUDAMemory::Ptr<char>  last_material_type;
	CUDAMemory::Ptr<float> last_pdf;

	inline void init(int buffer_size) {
		origin_x    = CUDAMemory::malloc<float>(buffer_size);
		origin_y    = CUDAMemory::malloc<float>(buffer_size);
		origin_z    = CUDAMemory::malloc<float>(buffer_size);
		direction_x = CUDAMemory::malloc<float>(buffer_size);
		direction_y = CUDAMemory::malloc<float>(buffer_size);
		direction_z = CUDAMemory::malloc<float>(buffer_size);

		pixel_index   = CUDAMemory::malloc<int>(buffer_size);
		throughput_x  = CUDAMemory::malloc<float>(buffer_size);
		throughput_y  = CUDAMemory::malloc<float>(buffer_size);
		throughput_z  = CUDAMemory::malloc<float>(buffer_size);

		last_material_type = CUDAMemory::malloc<char>(buffer_size);
		last_pdf           = CUDAMemory::malloc<float>(buffer_size);
	}
};
	
static struct MaterialBuffer {
	CUDAMemory::Ptr<float> direction_x;
	CUDAMemory::Ptr<float> direction_y;
	CUDAMemory::Ptr<float> direction_z;
	
	CUDAMemory::Ptr<int> triangle_id;
	CUDAMemory::Ptr<float> u;
	CUDAMemory::Ptr<float> v;

	CUDAMemory::Ptr<int>   pixel_index;
	CUDAMemory::Ptr<float> throughput_x;
	CUDAMemory::Ptr<float> throughput_y;
	CUDAMemory::Ptr<float> throughput_z;

	inline void init(int buffer_size) {
		direction_x = CUDAMemory::malloc<float>(buffer_size);
		direction_y = CUDAMemory::malloc<float>(buffer_size);
		direction_z = CUDAMemory::malloc<float>(buffer_size);

		triangle_id = CUDAMemory::malloc<int>(buffer_size);
		u = CUDAMemory::malloc<float>(buffer_size);
		v = CUDAMemory::malloc<float>(buffer_size);

		pixel_index   = CUDAMemory::malloc<int>(buffer_size);
		throughput_x  = CUDAMemory::malloc<float>(buffer_size);
		throughput_y  = CUDAMemory::malloc<float>(buffer_size);
		throughput_z  = CUDAMemory::malloc<float>(buffer_size);
	}
};
	
static struct ShadowRayBuffer {
	CUDAMemory::Ptr<float> direction_x;
	CUDAMemory::Ptr<float> direction_y;
	CUDAMemory::Ptr<float> direction_z;

	CUDAMemory::Ptr<int> triangle_id;
	CUDAMemory::Ptr<float> u;
	CUDAMemory::Ptr<float> v;

	CUDAMemory::Ptr<int> pixel_index;
	CUDAMemory::Ptr<float> throughput_x;
	CUDAMemory::Ptr<float> throughput_y;
	CUDAMemory::Ptr<float> throughput_z;

	inline void init(int buffer_size) {
		direction_x = CUDAMemory::malloc<float>(buffer_size);
		direction_y = CUDAMemory::malloc<float>(buffer_size);
		direction_z = CUDAMemory::malloc<float>(buffer_size);

		triangle_id = CUDAMemory::malloc<int>(buffer_size);
		u = CUDAMemory::malloc<float>(buffer_size);
		v = CUDAMemory::malloc<float>(buffer_size);

		pixel_index  = CUDAMemory::malloc<int>(buffer_size);
		throughput_x = CUDAMemory::malloc<float>(buffer_size);
		throughput_y = CUDAMemory::malloc<float>(buffer_size);
		throughput_z = CUDAMemory::malloc<float>(buffer_size);
	}
};

static struct BufferSizes {
	int N_extend    [NUM_BOUNCES] = { PIXEL_COUNT, 0 }; // On the first bounce the ExtendBuffer contains exactly PIXEL_COUNT Rays
	int N_diffuse   [NUM_BOUNCES] = { 0 };
	int N_dielectric[NUM_BOUNCES] = { 0 };
	int N_glossy    [NUM_BOUNCES] = { 0 };
	int N_shadow    [NUM_BOUNCES] = { 0 };
};

static BufferSizes buffer_sizes;

void Pathtracer::init(const char * scene_name, const char * sky_name, unsigned frame_buffer_handle) {
	CUDAContext::init();

	camera.init(DEG_TO_RAD(110.0f));
	camera.resize(SCREEN_WIDTH, SCREEN_HEIGHT);

	// Init CUDA Module and its Kernel
	module.init("CUDA_Source/wavefront.cu", CUDAContext::compute_capability);

	const MeshData * mesh = MeshData::load(scene_name);

	if (mesh->material_count > MAX_MATERIALS || Texture::texture_count > MAX_TEXTURES) abort();

	// Set global Material table
	module.get_global("materials").set_buffer(mesh->materials, mesh->material_count);

	// Set global Texture table
	if (Texture::texture_count > 0) {
		CUtexObject * tex_objects = new CUtexObject[Texture::texture_count];

		for (int i = 0; i < Texture::texture_count; i++) {
			CUarray array = CUDAMemory::create_array(Texture::textures[i].width, Texture::textures[i].height, Texture::textures[i].channels, CUarray_format::CU_AD_FORMAT_UNSIGNED_INT8);
		
			CUDAMemory::copy_array(array, Texture::textures[i].channels * Texture::textures[i].width, Texture::textures[i].height, Texture::textures[i].data);

			// Describe the Array to read from
			CUDA_RESOURCE_DESC res_desc = { };
			res_desc.resType = CUresourcetype::CU_RESOURCE_TYPE_ARRAY;
			res_desc.res.array.hArray = array;
		
			// Describe how to sample the Texture
			CUDA_TEXTURE_DESC tex_desc = { };
			tex_desc.addressMode[0] = CUaddress_mode::CU_TR_ADDRESS_MODE_WRAP;
			tex_desc.addressMode[1] = CUaddress_mode::CU_TR_ADDRESS_MODE_WRAP;
			tex_desc.filterMode = CUfilter_mode::CU_TR_FILTER_MODE_POINT;
			tex_desc.flags = CU_TRSF_NORMALIZED_COORDINATES | CU_TRSF_SRGB;
			
			CUDACALL(cuTexObjectCreate(tex_objects + i, &res_desc, &tex_desc, nullptr));
		}

		module.get_global("textures").set_buffer(tex_objects, Texture::texture_count);

		delete [] tex_objects;
	}

	// Construct BVH for the Triangle soup
	BVH<Triangle> bvh;

	std::string bvh_filename = std::string(scene_name) + ".bvh";
	if (std::filesystem::exists(bvh_filename)) {
		printf("Loading BVH %s from disk.\n", bvh_filename.c_str());

		bvh.load_from_disk(bvh_filename.c_str());
	} else {
		bvh.init(mesh->triangle_count);

		memcpy(bvh.primitives, mesh->triangles, mesh->triangle_count * sizeof(Triangle));

		for (int i = 0; i < bvh.primitive_count; i++) {
			Vector3 vertices[3] = { 
				bvh.primitives[i].position0, 
				bvh.primitives[i].position1, 
				bvh.primitives[i].position2
			};
			bvh.primitives[i].aabb = AABB::from_points(vertices, 3);
		}

		{
			ScopedTimer timer("SBVH Construction");

			bvh.build_sbvh();
		}

		bvh.save_to_disk(bvh_filename.c_str());
	}

	MBVH<Triangle> mbvh;
	mbvh.init(bvh);

	// Flatten the Primitives array so that we don't need the indices array as an indirection to index it
	// (This does mean more memory consumption)
	Triangle * flat_triangles = new Triangle[mbvh.leaf_count];
	for (int i = 0; i < mbvh.leaf_count; i++) {
		flat_triangles[i] = mbvh.primitives[mbvh.indices[i]];
	}

	// Allocate Triangles in SoA format
	Vector3 * triangles_position0      = new Vector3[mbvh.leaf_count];
	Vector3 * triangles_position_edge1 = new Vector3[mbvh.leaf_count];
	Vector3 * triangles_position_edge2 = new Vector3[mbvh.leaf_count];

	Vector3 * triangles_normal0      = new Vector3[mbvh.leaf_count];
	Vector3 * triangles_normal_edge1 = new Vector3[mbvh.leaf_count];
	Vector3 * triangles_normal_edge2 = new Vector3[mbvh.leaf_count]; 
	
	Vector2 * triangles_tex_coord0      = new Vector2[mbvh.leaf_count];
	Vector2 * triangles_tex_coord_edge1 = new Vector2[mbvh.leaf_count];
	Vector2 * triangles_tex_coord_edge2 = new Vector2[mbvh.leaf_count];

	int * triangles_material_id = new int[mbvh.leaf_count];

	for (int i = 0; i < mbvh.leaf_count; i++) {
		triangles_position0[i]      = flat_triangles[i].position0;
		triangles_position_edge1[i] = flat_triangles[i].position1 - flat_triangles[i].position0;
		triangles_position_edge2[i] = flat_triangles[i].position2 - flat_triangles[i].position0;

		triangles_normal0[i]      = flat_triangles[i].normal0;
		triangles_normal_edge1[i] = flat_triangles[i].normal1 - flat_triangles[i].normal0;
		triangles_normal_edge2[i] = flat_triangles[i].normal2 - flat_triangles[i].normal0;

		triangles_tex_coord0[i]      = flat_triangles[i].tex_coord0;
		triangles_tex_coord_edge1[i] = flat_triangles[i].tex_coord1 - flat_triangles[i].tex_coord0;
		triangles_tex_coord_edge2[i] = flat_triangles[i].tex_coord2 - flat_triangles[i].tex_coord0;

		triangles_material_id[i] = flat_triangles[i].material_id;
	}

	delete [] flat_triangles;

	// Set global Triangle buffers
	module.get_global("triangles_position0"     ).set_buffer(triangles_position0,      mbvh.leaf_count);
	module.get_global("triangles_position_edge1").set_buffer(triangles_position_edge1, mbvh.leaf_count);
	module.get_global("triangles_position_edge2").set_buffer(triangles_position_edge2, mbvh.leaf_count);

	module.get_global("triangles_normal0"     ).set_buffer(triangles_normal0,      mbvh.leaf_count);
	module.get_global("triangles_normal_edge1").set_buffer(triangles_normal_edge1, mbvh.leaf_count);
	module.get_global("triangles_normal_edge2").set_buffer(triangles_normal_edge2, mbvh.leaf_count);

	module.get_global("triangles_tex_coord0"     ).set_buffer(triangles_tex_coord0,      mbvh.leaf_count);
	module.get_global("triangles_tex_coord_edge1").set_buffer(triangles_tex_coord_edge1, mbvh.leaf_count);
	module.get_global("triangles_tex_coord_edge2").set_buffer(triangles_tex_coord_edge2, mbvh.leaf_count);

	module.get_global("triangles_material_id").set_buffer(triangles_material_id, mbvh.leaf_count);

	// Clean up buffers on Host side
	delete [] triangles_position0;  
	delete [] triangles_position_edge1;
	delete [] triangles_position_edge2;
	delete [] triangles_normal0;
	delete [] triangles_normal_edge1;
	delete [] triangles_normal_edge2;
	delete [] triangles_tex_coord0;  
	delete [] triangles_tex_coord_edge1;
	delete [] triangles_tex_coord_edge2;
	delete [] triangles_material_id;

	int * light_indices = new int[mesh->triangle_count];
	int   light_count = 0;

	// For every Triangle, check whether it is a Light based on its Material
	for (int i = 0; i < mesh->triangle_count; i++) {
		const Triangle & triangle = mesh->triangles[i];

		if (mesh->materials[triangle.material_id].type == Material::Type::LIGHT) {
			int index = INVALID;

			// Apply the BVH index permutation
			for (int j = 0; j < mbvh.leaf_count; j++) {
				if (mbvh.indices[j] == i) {
					index = j;

					break;
				}
			}

			assert(index != INVALID);

			light_indices[light_count++] = index;
		}
	}
	
	if (light_count > 0) {
		module.get_global("light_indices").set_buffer(light_indices, light_count);
	}

	delete [] light_indices;

	module.get_global("light_count").set_value(light_count);

	// Set global MBVHNode buffer
	module.get_global("mbvh_nodes").set_buffer(mbvh.nodes, mbvh.node_count);

	// Set Sky globals
	Sky sky;
	sky.init(sky_name);

	module.get_global("sky_size").set_value(sky.size);
	module.get_global("sky_data").set_buffer(sky.data, sky.size * sky.size);
	
	// Set Blue Noise Sampler globals
	module.get_global("sobol_256spp_256d").set_buffer(sobol_256spp_256d);
	module.get_global("scrambling_tile").set_buffer(scrambling_tile);
	module.get_global("ranking_tile").set_buffer(ranking_tile);
	
	// Set frame buffer to a CUDA resource mapping of the GL frame buffer texture
	module.set_surface("frame_buffer", CUDAMemory::create_array3d(SCREEN_WIDTH, SCREEN_HEIGHT, 1, 4, CUarray_format::CU_AD_FORMAT_FLOAT, CUDA_ARRAY3D_SURFACE_LDST));
	module.set_surface("accumulator", CUDAContext::map_gl_texture(frame_buffer_handle));

	ExtendBuffer    ray_buffer_extend;
	MaterialBuffer  ray_buffer_shade_diffuse;
	MaterialBuffer  ray_buffer_shade_dielectric;
	MaterialBuffer  ray_buffer_shade_glossy;
	ShadowRayBuffer ray_buffer_connect;

	ray_buffer_extend.init          (PIXEL_COUNT);
	ray_buffer_shade_diffuse.init   (PIXEL_COUNT);
	ray_buffer_shade_dielectric.init(PIXEL_COUNT);
	ray_buffer_shade_glossy.init    (PIXEL_COUNT);
	ray_buffer_connect.init         (PIXEL_COUNT);

	module.get_global("ray_buffer_extend").set_value          (ray_buffer_extend);
	module.get_global("ray_buffer_shade_diffuse").set_value   (ray_buffer_shade_diffuse);
	module.get_global("ray_buffer_shade_dielectric").set_value(ray_buffer_shade_dielectric);
	module.get_global("ray_buffer_shade_glossy").set_value    (ray_buffer_shade_glossy);
	module.get_global("ray_buffer_connect").set_value         (ray_buffer_connect);

	global_buffer_sizes = module.get_global("buffer_sizes");
	global_buffer_sizes.set_value(buffer_sizes);

	kernel_generate.init        (&module, "kernel_generate");
	kernel_extend.init          (&module, "kernel_extend");
	kernel_shade_diffuse.init   (&module, "kernel_shade_diffuse");
	kernel_shade_dielectric.init(&module, "kernel_shade_dielectric");
	kernel_shade_glossy.init    (&module, "kernel_shade_glossy");
	kernel_connect.init         (&module, "kernel_connect");
	kernel_accumulate.init      (&module, "kernel_accumulate");

	kernel_generate.set_block_dim        (128, 1, 1);
	kernel_extend.set_block_dim          (128, 1, 1);
	kernel_shade_diffuse.set_block_dim   (128, 1, 1);
	kernel_shade_dielectric.set_block_dim(128, 1, 1);
	kernel_shade_glossy.set_block_dim    (128, 1, 1);
	kernel_connect.set_block_dim         (128, 1, 1);
	kernel_accumulate.set_block_dim(32, 4, 1);

	kernel_generate.set_grid_dim        (PIXEL_COUNT / kernel_generate.block_dim_x,         1, 1);
	kernel_extend.set_grid_dim          (PIXEL_COUNT / kernel_extend.block_dim_x,           1, 1);
	kernel_shade_diffuse.set_grid_dim   (PIXEL_COUNT / kernel_shade_diffuse.block_dim_x,    1, 1);
	kernel_shade_dielectric.set_grid_dim(PIXEL_COUNT / kernel_shade_dielectric.block_dim_x, 1, 1);
	kernel_shade_glossy.set_grid_dim    (PIXEL_COUNT / kernel_shade_glossy.block_dim_x,     1, 1);
	kernel_connect.set_grid_dim         (PIXEL_COUNT / kernel_connect.block_dim_x,          1, 1);
	kernel_accumulate.set_grid_dim(
		SCREEN_WIDTH  / kernel_accumulate.block_dim_x, 
		SCREEN_HEIGHT / kernel_accumulate.block_dim_y,
		1
	);

	if (strcmp(scene_name, DATA_PATH("pica/pica.obj")) == 0) {
		camera.position = Vector3(-14.875896f, 5.407789f, 22.486183f);
		camera.rotation = Quaternion(0.000000f, 0.980876f, 0.000000f, 0.194635f);
	} else if (strcmp(scene_name, DATA_PATH("sponza/sponza.obj")) == 0) {
		camera.position = Vector3(2.698714f, 39.508224f, 15.633610f);
		camera.rotation = Quaternion(0.000000f, -0.891950f, 0.000000f, 0.452135f);
	} else if (strcmp(scene_name, DATA_PATH("scene.obj")) == 0) {
		camera.position = Vector3(-0.101589f, 0.613379f, 3.580916f);
		camera.rotation = Quaternion(-0.006744f, 0.992265f, -0.107043f, -0.062512f);
	} else if (strcmp(scene_name, DATA_PATH("cornellbox.obj")) == 0) {
		camera.position = Vector3(0.528027f, 1.004323f, 0.774033f);
		camera.rotation = Quaternion(0.035059f, -0.963870f, 0.208413f, 0.162142f);
	} else if (strcmp(scene_name, DATA_PATH("glossy.obj")) == 0) {
		camera.position = Vector3(9.467193f, 5.919240f, -0.646071f);
		camera.rotation = Quaternion(0.179088f, -0.677310f, 0.175366f, 0.691683f);
	} else {
		camera.position = Vector3(1.272743f, 3.097532f, 3.189943f);
		camera.rotation = Quaternion(0.000000f, 0.995683f, 0.000000f, -0.092814f);
	}
}

void Pathtracer::update(float delta, const unsigned char * keys) {
	camera.update(delta, keys);

	if (camera.moved) {
		frames_since_camera_moved = 0;
	} else {
		frames_since_camera_moved++;
	}
}

void Pathtracer::render() {
	// Generate primary Rays from the current Camera orientation
	kernel_generate.execute(
		rand(),
		frames_since_camera_moved,
		camera.position, 
		camera.top_left_corner_rotated, 
		camera.x_axis_rotated, 
		camera.y_axis_rotated
	);

	global_buffer_sizes.set_value(buffer_sizes);

	for (int bounce = 0; bounce < NUM_BOUNCES; bounce++) {
		// Extend all Rays that are still alive to their next Triangle intersection
		kernel_extend.execute(rand(), bounce);

		// Process the various Material types in different Kernels
		kernel_shade_diffuse.execute   (rand(), bounce, frames_since_camera_moved);
		kernel_shade_dielectric.execute(rand(), bounce);
		kernel_shade_glossy.execute    (rand(), bounce, frames_since_camera_moved);

		// Trace shadow Rays
		kernel_connect.execute(rand(), frames_since_camera_moved, bounce);
	}

	kernel_accumulate.execute(float(frames_since_camera_moved));
}