/* Copyright (C) 2021 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "precompiled.h"

#include "CMiniMap.h"

#include "graphics/GameView.h"
#include "graphics/LOSTexture.h"
#include "graphics/MiniPatch.h"
#include "graphics/Terrain.h"
#include "graphics/TerrainTextureEntry.h"
#include "graphics/TerrainTextureManager.h"
#include "graphics/TerritoryTexture.h"
#include "gui/CGUI.h"
#include "gui/GUIManager.h"
#include "gui/GUIMatrix.h"
#include "lib/bits.h"
#include "lib/external_libraries/libsdl.h"
#include "lib/ogl.h"
#include "lib/timer.h"
#include "ps/ConfigDB.h"
#include "ps/Filesystem.h"
#include "ps/Game.h"
#include "ps/GameSetup/Config.h"
#include "ps/Profile.h"
#include "ps/World.h"
#include "ps/XML/Xeromyces.h"
#include "renderer/Renderer.h"
#include "renderer/RenderingOptions.h"
#include "renderer/WaterManager.h"
#include "scriptinterface/ScriptInterface.h"
#include "simulation2/Simulation2.h"
#include "simulation2/components/ICmpMinimap.h"
#include "simulation2/components/ICmpRangeManager.h"
#include "simulation2/helpers/Los.h"
#include "simulation2/system/ParamNode.h"

#include <array>
#include <cmath>
#include <vector>

extern bool g_GameRestarted;

namespace
{

// Set max drawn entities to UINT16_MAX for now, which is more than enough
// TODO: we should be cleverer about drawing them to reduce clutter
const u16 MAX_ENTITIES_DRAWN = 65535;

unsigned int ScaleColor(unsigned int color, float x)
{
	unsigned int r = unsigned(float(color & 0xff) * x);
	unsigned int g = unsigned(float((color>>8) & 0xff) * x);
	unsigned int b = unsigned(float((color>>16) & 0xff) * x);
	return (0xff000000 | b | g<<8 | r<<16);
}

// Adds segments pieces lying inside the circle to lines.
void CropPointsByCircle(const std::array<CVector3D, 4>& points, const CVector3D& center, const float radius, std::vector<CVector3D>* lines)
{
	constexpr float EPS = 1e-3f;
	const float radiusSquared = radius * radius;
	lines->reserve(points.size() * 2);
	for (size_t idx = 0; idx < points.size(); ++idx)
	{
		const CVector3D& currentPoint = points[idx];
		const CVector3D& nextPoint = points[(idx + 1) % points.size()];
		const CVector3D direction = (nextPoint - currentPoint).Normalized();
		const CVector3D normal(direction.Z, 0.0f, -direction.X);
		const float offset = normal.Dot(currentPoint) - normal.Dot(center);
		// We need to have lines only inside the circle.
		if (std::abs(offset) + EPS >= radius)
			continue;
		const CVector3D closestPoint = center + normal * offset;
		const float halfChordLength = sqrt(radius * radius - offset * offset);
		const CVector3D intersectionA = closestPoint - direction * halfChordLength;
		const CVector3D intersectionB = closestPoint + direction * halfChordLength;
		// We have no intersection if the segment is lying outside of the circle.
		if (direction.Dot(currentPoint) + EPS > direction.Dot(intersectionB) ||
		    direction.Dot(nextPoint) - EPS < direction.Dot(intersectionA))
			continue;

		lines->emplace_back(
			direction.Dot(currentPoint) > direction.Dot(intersectionA) ? currentPoint : intersectionA);
		lines->emplace_back(
			direction.Dot(nextPoint) < direction.Dot(intersectionB) ? nextPoint : intersectionB);
	}
}

} // anonymous namespace

const CStr CMiniMap::EventNameWorldClick = "WorldClick";

CMiniMap::CMiniMap(CGUI& pGUI) :
	IGUIObject(pGUI),
	m_TerrainTexture(0), m_TerrainData(0), m_MapSize(0), m_Terrain(0), m_TerrainDirty(true), m_MapScale(1.f),
	m_EntitiesDrawn(0), m_IndexArray(GL_STATIC_DRAW), m_VertexArray(GL_DYNAMIC_DRAW), m_Mask(false),
	m_NextBlinkTime(0.0), m_PingDuration(25.0), m_BlinkState(false), m_WaterHeight(0.0)
{
	RegisterSetting("mask", m_Mask);

	m_Clicking = false;
	m_MouseHovering = false;

	// Register Relax NG validator
	CXeromyces::AddValidator(g_VFS, "pathfinder", "simulation/data/pathfinder.rng");

	m_ShallowPassageHeight = GetShallowPassageHeight();

	m_AttributePos.type = GL_FLOAT;
	m_AttributePos.elems = 2;
	m_VertexArray.AddAttribute(&m_AttributePos);

	m_AttributeColor.type = GL_UNSIGNED_BYTE;
	m_AttributeColor.elems = 4;
	m_VertexArray.AddAttribute(&m_AttributeColor);

	m_VertexArray.SetNumVertices(MAX_ENTITIES_DRAWN);
	m_VertexArray.Layout();

	m_IndexArray.SetNumVertices(MAX_ENTITIES_DRAWN);
	m_IndexArray.Layout();
	VertexArrayIterator<u16> index = m_IndexArray.GetIterator();
	for (u16 i = 0; i < MAX_ENTITIES_DRAWN; ++i)
		*index++ = i;
	m_IndexArray.Upload();
	m_IndexArray.FreeBackingStore();


	VertexArrayIterator<float[2]> attrPos = m_AttributePos.GetIterator<float[2]>();
	VertexArrayIterator<u8[4]> attrColor = m_AttributeColor.GetIterator<u8[4]>();
	for (u16 i = 0; i < MAX_ENTITIES_DRAWN; ++i)
	{
		(*attrColor)[0] = 0;
		(*attrColor)[1] = 0;
		(*attrColor)[2] = 0;
		(*attrColor)[3] = 0;
		++attrColor;

		(*attrPos)[0] = -10000.0f;
		(*attrPos)[1] = -10000.0f;

		++attrPos;

	}
	m_VertexArray.Upload();

	double blinkDuration = 1.0;

	// Tests won't have config initialised
	if (CConfigDB::IsInitialised())
	{
		CFG_GET_VAL("gui.session.minimap.pingduration", m_PingDuration);
		CFG_GET_VAL("gui.session.minimap.blinkduration", blinkDuration);
	}
	m_HalfBlinkDuration = blinkDuration/2;
}

CMiniMap::~CMiniMap()
{
	Destroy();
}

void CMiniMap::HandleMessage(SGUIMessage& Message)
{
	IGUIObject::HandleMessage(Message);
	switch (Message.type)
	{
	case GUIM_MOUSE_PRESS_LEFT:
		if (m_MouseHovering)
		{
			if (!CMiniMap::FireWorldClickEvent(SDL_BUTTON_LEFT, 1))
			{
				SetCameraPos();
				m_Clicking = true;
			}
		}
		break;
	case GUIM_MOUSE_RELEASE_LEFT:
		if (m_MouseHovering && m_Clicking)
			SetCameraPos();
		m_Clicking = false;
		break;
	case GUIM_MOUSE_DBLCLICK_LEFT:
		if (m_MouseHovering && m_Clicking)
			SetCameraPos();
		m_Clicking = false;
		break;
	case GUIM_MOUSE_ENTER:
		m_MouseHovering = true;
		break;
	case GUIM_MOUSE_LEAVE:
		m_Clicking = false;
		m_MouseHovering = false;
		break;
	case GUIM_MOUSE_RELEASE_RIGHT:
		CMiniMap::FireWorldClickEvent(SDL_BUTTON_RIGHT, 1);
		break;
	case GUIM_MOUSE_DBLCLICK_RIGHT:
		CMiniMap::FireWorldClickEvent(SDL_BUTTON_RIGHT, 2);
		break;
	case GUIM_MOUSE_MOTION:
		if (m_MouseHovering && m_Clicking)
			SetCameraPos();
		break;
	case GUIM_MOUSE_WHEEL_DOWN:
	case GUIM_MOUSE_WHEEL_UP:
		Message.Skip();
		break;

	default:
		break;
	}
}

bool CMiniMap::IsMouseOver() const
{
	// Get the mouse position.
	const CVector2D& mousePos = m_pGUI.GetMousePos();
	// Get the position of the center of the minimap.
	CVector2D minimapCenter = CVector2D(m_CachedActualSize.left + m_CachedActualSize.GetWidth() / 2.0, m_CachedActualSize.bottom - m_CachedActualSize.GetHeight() / 2.0);
	// Take the magnitude of the difference of the mouse position and minimap center.
	double distFromCenter = sqrt(pow((mousePos.X - minimapCenter.X), 2) + pow((mousePos.Y - minimapCenter.Y), 2));
	// If the distance is less then the radius of the minimap (half the width) the mouse is over the minimap.
	if (distFromCenter < m_CachedActualSize.GetWidth() / 2.0)
		return true;
	else
		return false;
}

void CMiniMap::GetMouseWorldCoordinates(float& x, float& z) const
{
	// Determine X and Z according to proportion of mouse position and minimap

	const CVector2D& mousePos = m_pGUI.GetMousePos();

	float px = (mousePos.X - m_CachedActualSize.left) / m_CachedActualSize.GetWidth();
	float py = (m_CachedActualSize.bottom - mousePos.Y) / m_CachedActualSize.GetHeight();

	float angle = GetAngle();

	// Scale world coordinates for shrunken square map
	x = TERRAIN_TILE_SIZE * m_MapSize * (m_MapScale * (cos(angle)*(px-0.5) - sin(angle)*(py-0.5)) + 0.5);
	z = TERRAIN_TILE_SIZE * m_MapSize * (m_MapScale * (cos(angle)*(py-0.5) + sin(angle)*(px-0.5)) + 0.5);
}

void CMiniMap::SetCameraPos()
{
	CTerrain* terrain = g_Game->GetWorld()->GetTerrain();

	CVector3D target;
	GetMouseWorldCoordinates(target.X, target.Z);
	target.Y = terrain->GetExactGroundLevel(target.X, target.Z);
	g_Game->GetView()->MoveCameraTarget(target);
}

float CMiniMap::GetAngle() const
{
	CVector3D cameraIn = m_Camera->GetOrientation().GetIn();
	return -atan2(cameraIn.X, cameraIn.Z);
}

bool CMiniMap::FireWorldClickEvent(int button, int UNUSED(clicks))
{
	ScriptRequest rq(g_GUI->GetActiveGUI()->GetScriptInterface());

	float x, z;
	GetMouseWorldCoordinates(x, z);

	JS::RootedValue coords(rq.cx);
	ScriptInterface::CreateObject(rq, &coords, "x", x, "z", z);

	JS::RootedValue buttonJs(rq.cx);
	ScriptInterface::ToJSVal(rq, &buttonJs, button);

	JS::RootedValueVector paramData(rq.cx);
	ignore_result(paramData.append(coords));
	ignore_result(paramData.append(buttonJs));

	return ScriptEventWithReturn(EventNameWorldClick, paramData);
}

// This sets up and draws the rectangle on the minimap
//  which represents the view of the camera in the world.
void CMiniMap::DrawViewRect(const CMatrix3D& transform) const
{
	// Compute the camera frustum intersected with a fixed-height plane.
	// Use the water height as a fixed base height, which should be the lowest we can go
	float h = g_Renderer.GetWaterManager()->m_WaterHeight;
	const float width = m_CachedActualSize.GetWidth();
	const float height = m_CachedActualSize.GetHeight();
	const float invTileMapSize = 1.0f / float(TERRAIN_TILE_SIZE * m_MapSize);

	const std::array<CVector3D, 4> hitPoints = {
		m_Camera->GetWorldCoordinates(0, g_Renderer.GetHeight(), h),
		m_Camera->GetWorldCoordinates(g_Renderer.GetWidth(), g_Renderer.GetHeight(), h),
		m_Camera->GetWorldCoordinates(g_Renderer.GetWidth(), 0, h),
		m_Camera->GetWorldCoordinates(0, 0, h)
	};

	std::vector<CVector3D> lines;
	// We need to prevent drawing view bounds out of the map.
	const float halfMapSize = static_cast<float>((m_MapSize - 1) * TERRAIN_TILE_SIZE) * 0.5f;
	CropPointsByCircle(hitPoints, CVector3D(halfMapSize, 0.0f, halfMapSize), halfMapSize, &lines);
	if (lines.empty())
		return;

	std::vector<float> vertices;
	vertices.reserve(lines.size() * 2);
	for (const CVector3D& point : lines)
	{
		// Convert to minimap space.
		vertices.emplace_back(width * point.X * invTileMapSize);
		vertices.emplace_back(-(height * point.Z * invTileMapSize));
	}

	// Enable Scissoring to restrict the rectangle to only the minimap.
	glScissor(
		m_CachedActualSize.left * g_GuiScale,
		g_Renderer.GetHeight() - m_CachedActualSize.bottom * g_GuiScale,
		width * g_GuiScale,
		height * g_GuiScale);
	glEnable(GL_SCISSOR_TEST);
	glLineWidth(2.0f);

	CShaderDefines lineDefines;
	lineDefines.Add(str_MINIMAP_LINE, str_1);
	CShaderTechniquePtr tech = g_Renderer.GetShaderManager().LoadEffect(str_minimap, g_Renderer.GetSystemShaderDefines(), lineDefines);
	tech->BeginPass();
	CShaderProgramPtr shader = tech->GetShader();
	shader->Uniform(str_transform, transform);
	shader->Uniform(str_color, 1.0f, 0.3f, 0.3f, 1.0f);

	shader->VertexPointer(2, GL_FLOAT, 0, vertices.data());
	shader->AssertPointersBound();

	if (!g_Renderer.m_SkipSubmit)
		glDrawArrays(GL_LINES, 0, vertices.size() / 2);

	tech->EndPass();

	glLineWidth(1.0f);
	glDisable(GL_SCISSOR_TEST);
}

struct MinimapUnitVertex
{
	// This struct is copyable for convenience and because to move is to copy for primitives.
	u8 r, g, b, a;
	float x, y;
};

// Adds a vertex to the passed VertexArray
static void inline addVertex(const MinimapUnitVertex& v,
					  VertexArrayIterator<u8[4]>& attrColor,
					  VertexArrayIterator<float[2]>& attrPos)
{
	(*attrColor)[0] = v.r;
	(*attrColor)[1] = v.g;
	(*attrColor)[2] = v.b;
	(*attrColor)[3] = v.a;
	++attrColor;

	(*attrPos)[0] = v.x;
	(*attrPos)[1] = v.y;

	++attrPos;
}


void CMiniMap::DrawTexture(CShaderProgramPtr shader, float coordMax, float angle, float x, float y, float x2, float y2, float z) const
{
	// Rotate the texture coordinates (0,0)-(coordMax,coordMax) around their center point (m,m)
	// Scale square maps to fit in circular minimap area
	const float s = sin(angle) * m_MapScale;
	const float c = cos(angle) * m_MapScale;
	const float m = coordMax / 2.f;

	float quadTex[] = {
		m*(-c + s + 1.f), m*(-c + -s + 1.f),
		m*(c + s + 1.f), m*(-c + s + 1.f),
		m*(c + -s + 1.f), m*(c + s + 1.f),

		m*(c + -s + 1.f), m*(c + s + 1.f),
		m*(-c + -s + 1.f), m*(c + -s + 1.f),
		m*(-c + s + 1.f), m*(-c + -s + 1.f)
	};
	float quadVerts[] = {
		x, y, z,
		x2, y, z,
		x2, y2, z,

		x2, y2, z,
		x, y2, z,
		x, y, z
	};

	shader->TexCoordPointer(GL_TEXTURE0, 2, GL_FLOAT, 0, quadTex);
	shader->VertexPointer(3, GL_FLOAT, 0, quadVerts);
	shader->AssertPointersBound();

	if (!g_Renderer.m_SkipSubmit)
		glDrawArrays(GL_TRIANGLES, 0, 6);
}

// TODO: render the minimap in a framebuffer and just draw the frambuffer texture
//	most of the time, updating the framebuffer twice a frame.
// Here it updates as ping-pong either texture or vertex array each sec to lower gpu stalling
// (those operations cause a gpu sync, which slows down the way gpu works)
void CMiniMap::Draw()
{
	PROFILE3("render minimap");

	// The terrain isn't actually initialized until the map is loaded, which
	// happens when the game is started, so abort until then.
	if (!g_Game || !g_Game->IsGameStarted())
		return;

	CSimulation2* sim = g_Game->GetSimulation2();
	CmpPtr<ICmpRangeManager> cmpRangeManager(*sim, SYSTEM_ENTITY);
	ENSURE(cmpRangeManager);

	// Set our globals in case they hadn't been set before
	m_Camera = g_Game->GetView()->GetCamera();
	m_Terrain = g_Game->GetWorld()->GetTerrain();
	m_Width  = (u32)(m_CachedActualSize.right - m_CachedActualSize.left);
	m_Height = (u32)(m_CachedActualSize.bottom - m_CachedActualSize.top);
	m_MapSize = m_Terrain->GetVerticesPerSide();
	m_TextureSize = (GLsizei)round_up_to_pow2((size_t)m_MapSize);
	m_MapScale = (cmpRangeManager->GetLosCircular() ? 1.f : 1.414f);

	if (!m_TerrainTexture || g_GameRestarted)
		CreateTextures();


	// only update 2x / second
	// (note: since units only move a few pixels per second on the minimap,
	// we can get away with infrequent updates; this is slow)
	// TODO: Update all but camera at same speed as simulation
	static double last_time;
	const double cur_time = timer_Time();
	const bool doUpdate = cur_time - last_time > 0.5;
	if (doUpdate)
	{
		last_time = cur_time;
		if (m_TerrainDirty || m_WaterHeight != g_Renderer.GetWaterManager()->m_WaterHeight)
			RebuildTerrainTexture();
	}

	const float x = m_CachedActualSize.left, y = m_CachedActualSize.bottom;
	const float x2 = m_CachedActualSize.right, y2 = m_CachedActualSize.top;
	const float z = GetBufferedZ();
	const float texCoordMax = (float)(m_MapSize - 1) / (float)m_TextureSize;
	const float angle = GetAngle();
	const float unitScale = (cmpRangeManager->GetLosCircular() ? 1.f : m_MapScale/2.f);

	CLOSTexture& losTexture = g_Game->GetView()->GetLOSTexture();

	CShaderProgramPtr shader;
	CShaderTechniquePtr tech;

	CShaderDefines baseDefines;
	baseDefines.Add(str_MINIMAP_BASE, str_1);
	if (m_Mask)
		baseDefines.Add(str_MINIMAP_MASK, str_1);

	tech = g_Renderer.GetShaderManager().LoadEffect(str_minimap, g_Renderer.GetSystemShaderDefines(), baseDefines);
	tech->BeginPass();
	shader = tech->GetShader();

	// Draw the main textured quad
	shader->BindTexture(str_baseTex, m_TerrainTexture);
	if (m_Mask)
	{
		shader->BindTexture(str_maskTex, losTexture.GetTexture());
		CMatrix3D maskTextureTransform = *losTexture.GetMinimapTextureMatrix();
		// We need to have texture coordinates in the same coordinate space.
		const float scale = 1.0f / texCoordMax;
		maskTextureTransform.Scale(scale, scale, 1.0f);
		shader->Uniform(str_maskTextureTransform, maskTextureTransform);
	}
	const CMatrix3D baseTransform = GetDefaultGuiMatrix();
	CMatrix3D baseTextureTransform;
	baseTextureTransform.SetIdentity();
	shader->Uniform(str_transform, baseTransform);
	shader->Uniform(str_textureTransform, baseTextureTransform);

	if (m_Mask)
	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}

	DrawTexture(shader, texCoordMax, angle, x, y, x2, y2, z);

	if (!m_Mask)
	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}

	// Draw territory boundaries
	CTerritoryTexture& territoryTexture = g_Game->GetView()->GetTerritoryTexture();

	shader->BindTexture(str_baseTex, territoryTexture.GetTexture());
	if (m_Mask)
	{
		shader->BindTexture(str_maskTex, losTexture.GetTexture());
		shader->Uniform(str_maskTextureTransform, *losTexture.GetMinimapTextureMatrix());
	}
	const CMatrix3D* territoryTransform = territoryTexture.GetMinimapTextureMatrix();
	shader->Uniform(str_transform, baseTransform);
	shader->Uniform(str_textureTransform, *territoryTransform);

	DrawTexture(shader, 1.0f, angle, x, y, x2, y2, z);
	tech->EndPass();

	// Draw the LOS quad in black, using alpha values from the LOS texture
	if (!m_Mask)
	{
		CShaderDefines losDefines;
		losDefines.Add(str_MINIMAP_LOS, str_1);
		tech = g_Renderer.GetShaderManager().LoadEffect(str_minimap, g_Renderer.GetSystemShaderDefines(), losDefines);
		tech->BeginPass();
		shader = tech->GetShader();
		shader->BindTexture(str_baseTex, losTexture.GetTexture());

		const CMatrix3D* losTransform = losTexture.GetMinimapTextureMatrix();
		shader->Uniform(str_transform, baseTransform);
		shader->Uniform(str_textureTransform, *losTransform);

		DrawTexture(shader, 1.0f, angle, x, y, x2, y2, z);
		tech->EndPass();
	}

	glDisable(GL_BLEND);

	PROFILE_START("minimap units");

	CShaderDefines pointDefines;
	pointDefines.Add(str_MINIMAP_POINT, str_1);
	tech = g_Renderer.GetShaderManager().LoadEffect(str_minimap, g_Renderer.GetSystemShaderDefines(), pointDefines);
	tech->BeginPass();
	shader = tech->GetShader();
	shader->Uniform(str_transform, baseTransform);
	shader->Uniform(str_pointSize, 3.f);

	CMatrix3D unitMatrix;
	unitMatrix.SetIdentity();
	// Center the minimap on the origin of the axis of rotation.
	unitMatrix.Translate(-(x2 - x) / 2.f, -(y2 - y) / 2.f, 0.f);
	// Rotate the map.
	unitMatrix.RotateZ(angle);
	// Scale square maps to fit.
	unitMatrix.Scale(unitScale, unitScale, 1.f);
	// Move the minimap back to it's starting position.
	unitMatrix.Translate((x2 - x) / 2.f, (y2 - y) / 2.f, 0.f);
	// Move the minimap to it's final location.
	unitMatrix.Translate(x, y, z);
	// Apply the gui matrix.
	unitMatrix *= GetDefaultGuiMatrix();
	// Load the transform into the shader.
	shader->Uniform(str_transform, unitMatrix);

	const float sx = (float)m_Width / ((m_MapSize - 1) * TERRAIN_TILE_SIZE);
	const float sy = (float)m_Height / ((m_MapSize - 1) * TERRAIN_TILE_SIZE);

	CSimulation2::InterfaceList ents = sim->GetEntitiesWithInterface(IID_Minimap);

	if (doUpdate)
	{
		VertexArrayIterator<float[2]> attrPos = m_AttributePos.GetIterator<float[2]>();
		VertexArrayIterator<u8[4]> attrColor = m_AttributeColor.GetIterator<u8[4]>();

		m_EntitiesDrawn = 0;
		MinimapUnitVertex v;
		std::vector<MinimapUnitVertex> pingingVertices;
		pingingVertices.reserve(MAX_ENTITIES_DRAWN / 2);

		if (cur_time > m_NextBlinkTime)
		{
			m_BlinkState = !m_BlinkState;
			m_NextBlinkTime = cur_time + m_HalfBlinkDuration;
		}

		entity_pos_t posX, posZ;
		for (CSimulation2::InterfaceList::const_iterator it = ents.begin(); it != ents.end(); ++it)
		{
			ICmpMinimap* cmpMinimap = static_cast<ICmpMinimap*>(it->second);
			if (cmpMinimap->GetRenderData(v.r, v.g, v.b, posX, posZ))
			{
				LosVisibility vis = cmpRangeManager->GetLosVisibility(it->first, g_Game->GetSimulation2()->GetSimContext().GetCurrentDisplayedPlayer());
				if (vis != LosVisibility::HIDDEN)
				{
					v.a = 255;
					v.x = posX.ToFloat() * sx;
					v.y = -posZ.ToFloat() * sy;

					// Check minimap pinging to indicate something
					if (m_BlinkState && cmpMinimap->CheckPing(cur_time, m_PingDuration))
					{
						v.r = 255; // ping color is white
						v.g = 255;
						v.b = 255;
						pingingVertices.push_back(v);
					}
					else
					{
						addVertex(v, attrColor, attrPos);
						++m_EntitiesDrawn;
					}
				}
			}
		}

		// Add the pinged vertices at the end, so they are drawn on top
		for (const MinimapUnitVertex& vertex : pingingVertices)
		{
			addVertex(vertex, attrColor, attrPos);
			++m_EntitiesDrawn;
		}

		ENSURE(m_EntitiesDrawn < MAX_ENTITIES_DRAWN);
		m_VertexArray.Upload();
	}

	m_VertexArray.PrepareForRendering();

	if (m_EntitiesDrawn > 0)
	{
#if !CONFIG2_GLES
		glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
#endif

		u8* indexBase = m_IndexArray.Bind();
		u8* base = m_VertexArray.Bind();
		const GLsizei stride = (GLsizei)m_VertexArray.GetStride();

		shader->VertexPointer(2, GL_FLOAT, stride, base + m_AttributePos.offset);
		shader->ColorPointer(4, GL_UNSIGNED_BYTE, stride, base + m_AttributeColor.offset);
		shader->AssertPointersBound();

		if (!g_Renderer.m_SkipSubmit)
			glDrawElements(GL_POINTS, (GLsizei)(m_EntitiesDrawn), GL_UNSIGNED_SHORT, indexBase);

		g_Renderer.GetStats().m_DrawCalls++;
		CVertexBuffer::Unbind();

#if !CONFIG2_GLES
		glDisable(GL_VERTEX_PROGRAM_POINT_SIZE);
#endif
	}

	tech->EndPass();

	DrawViewRect(unitMatrix);

	PROFILE_END("minimap units");
}

void CMiniMap::CreateTextures()
{
	Destroy();

	// Create terrain texture
	glGenTextures(1, &m_TerrainTexture);
	g_Renderer.BindTexture(0, m_TerrainTexture);

	// Initialise texture with solid black, for the areas we don't
	// overwrite with glTexSubImage2D later
	u32* texData = new u32[m_TextureSize * m_TextureSize];
	for (ssize_t i = 0; i < m_TextureSize * m_TextureSize; ++i)
		texData[i] = 0xFF000000;
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_TextureSize, m_TextureSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, texData);
	delete[] texData;

	m_TerrainData = new u32[(m_MapSize - 1) * (m_MapSize - 1)];
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// Rebuild and upload both of them
	RebuildTerrainTexture();
}


void CMiniMap::RebuildTerrainTexture()
{
	u32 x = 0;
	u32 y = 0;
	u32 w = m_MapSize - 1;
	u32 h = m_MapSize - 1;
	m_WaterHeight = g_Renderer.GetWaterManager()->m_WaterHeight;

	m_TerrainDirty = false;

	for (u32 j = 0; j < h; ++j)
	{
		u32* dataPtr = m_TerrainData + ((y + j) * (m_MapSize - 1)) + x;
		for (u32 i = 0; i < w; ++i)
		{
			float avgHeight = ( m_Terrain->GetVertexGroundLevel((int)i, (int)j)
					+ m_Terrain->GetVertexGroundLevel((int)i+1, (int)j)
					+ m_Terrain->GetVertexGroundLevel((int)i, (int)j+1)
					+ m_Terrain->GetVertexGroundLevel((int)i+1, (int)j+1)
				) / 4.0f;

			if (avgHeight < m_WaterHeight && avgHeight > m_WaterHeight - m_ShallowPassageHeight)
			{
				// shallow water
				*dataPtr++ = 0xffc09870;
			}
			else if (avgHeight < m_WaterHeight)
			{
				// Set water as constant color for consistency on different maps
				*dataPtr++ = 0xffa07850;
			}
			else
			{
				int hmap = ((int)m_Terrain->GetHeightMap()[(y + j) * m_MapSize + x + i]) >> 8;
				int val = (hmap / 3) + 170;

				u32 color = 0xFFFFFFFF;

				CMiniPatch* mp = m_Terrain->GetTile(x + i, y + j);
				if (mp)
				{
					CTerrainTextureEntry* tex = mp->GetTextureEntry();
					if (tex)
					{
						// If the texture can't be loaded yet, set the dirty flags
						// so we'll try regenerating the terrain texture again soon
						if(!tex->GetTexture()->TryLoad())
							m_TerrainDirty = true;

						color = tex->GetBaseColor();
					}
				}

				*dataPtr++ = ScaleColor(color, float(val) / 255.0f);
			}
		}
	}

	// Upload the texture
	g_Renderer.BindTexture(0, m_TerrainTexture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_MapSize - 1, m_MapSize - 1, GL_RGBA, GL_UNSIGNED_BYTE, m_TerrainData);
}

void CMiniMap::Destroy()
{
	if (m_TerrainTexture)
	{
		glDeleteTextures(1, &m_TerrainTexture);
		m_TerrainTexture = 0;
	}

	SAFE_ARRAY_DELETE(m_TerrainData);
}

// static
float CMiniMap::GetShallowPassageHeight()
{
	float shallowPassageHeight = 0.0f;
	CParamNode externalParamNode;
	CParamNode::LoadXML(externalParamNode, L"simulation/data/pathfinder.xml", "pathfinder");
	const CParamNode pathingSettings = externalParamNode.GetChild("Pathfinder").GetChild("PassabilityClasses");
	if (pathingSettings.GetChild("default").IsOk() && pathingSettings.GetChild("default").GetChild("MaxWaterDepth").IsOk())
		shallowPassageHeight = pathingSettings.GetChild("default").GetChild("MaxWaterDepth").ToFloat();
	return shallowPassageHeight;
}
