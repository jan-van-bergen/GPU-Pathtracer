#pragma once
#include "Texture.h"

struct Material {
	Vector3 diffuse = Vector3(1.0f, 1.0f, 1.0f);	
	const Texture * texture = nullptr;

	Vector3 reflection;

	Vector3 transmittance;
	float   index_of_refraction = 1.0f;
};
