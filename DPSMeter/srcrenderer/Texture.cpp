#include <d3d9.h>
// D3dx9tex.h removed - not available in Windows SDK
// D3DX texture functions stubbed; overlay falls back to text buttons
#include <algorithm>

#include "Texture.h"
#include "proc.h"
#include "Logger.h"
#include "resource.h"

//=============================================================================
//=============================================================================
#define DPSMETERDLL "DPSMeter.dll"

//=============================================================================
//=============================================================================
void* Texture::LoadTextureFromFile(const char* filename)
{
  // D3DX stub - not available without DirectX SDK
  // Overlay falls back to text-based buttons
  return NULL;
}

void* Texture::LoadTextureFromResource(unsigned int idb)
{
  // D3DX stub - not available without DirectX SDK
  // Overlay falls back to text-based buttons
  return NULL;
}

