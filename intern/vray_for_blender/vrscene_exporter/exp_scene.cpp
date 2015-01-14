/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Andrei Izrantcev <andrei.izrantcev@chaosgroup.com>
 *
 * * ***** END GPL LICENSE BLOCK *****
 */

#include "cgr_config.h"

#include "GeomMayaHair.h"
#include "GeomStaticMesh.h"

#include "cgr_vrscene.h"
#include "cgr_string.h"
#include "cgr_blender_data.h"
#include "cgr_json_plugins.h"
#include "cgr_rna.h"
#include "cgr_paths.h"

#include "exp_scene.h"
#include "exp_nodes.h"

#include "PIL_time.h"
#include "BLI_string.h"
#include "BKE_material.h"
#include "BKE_global.h"
#include "BLI_math_matrix.h"
#include "BLI_utildefines.h"

extern "C" {
#  include "RE_engine.h"
#  include "DNA_particle_types.h"
#  include "DNA_modifier_types.h"
#  include "DNA_material_types.h"
#  include "DNA_lamp_types.h"
#  include "DNA_camera_types.h"
#  include "BKE_anim.h"
#  include "DNA_windowmanager_types.h"
}

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/join.hpp>


// Default velocity transform matrix hex
const char* MyParticle::velocity = "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";


static int IsDuplicatorRenderable(BL::Object ob)
{
	if(NOT(ob.is_duplicator()))
		return true;

	if(ob.dupli_type() != BL::Object::dupli_type_NONE)
		return false;

	if(ob.particle_systems.length()) {
		BL::Object::particle_systems_iterator psysIt;
		for(ob.particle_systems.begin(psysIt); psysIt != ob.particle_systems.end(); ++psysIt) {
			BL::ParticleSystem   psys = *psysIt;
			BL::ParticleSettings pset = psys.settings();
			if(pset.use_render_emitter())
				return true;
		}
	}

	return false;
}


VRsceneExporter::VRsceneExporter()
{
	PRINT_INFO("VRsceneExporter::VRsceneExporter()");

	exportSceneInit();
}


VRsceneExporter::~VRsceneExporter()
{
	PRINT_INFO("VRsceneExporter::~VRsceneExporter()");

	m_skipObjects.clear();
}


void VRsceneExporter::addSkipObject(void *obPtr)
{
	m_skipObjects.insert(obPtr);
}


void VRsceneExporter::addToHideFromViewList(const std::string &listKey, void *obPtr)
{
	PRINT_INFO("Adding object '%s' to hide list '%s'...",
			   ((ID*)obPtr)->name, listKey.c_str());

	if(listKey == "all")
		m_hideFromView.visibility.insert(obPtr);
	else if (listKey == "camera")
		m_hideFromView.camera_visibility.insert(obPtr);
	else if (listKey == "gi")
		m_hideFromView.gi_visibility.insert(obPtr);
	else if (listKey == "reflect")
		m_hideFromView.reflections_visibility.insert(obPtr);
	else if (listKey == "refract")
		m_hideFromView.refractions_visibility.insert(obPtr);
	else if (listKey == "shadows")
		m_hideFromView.shadows_visibility.insert(obPtr);
}


void VRsceneExporter::exportSceneInit()
{
	VRayExportable::clearCache();
	ExporterSettings::gSet.init();

	// Prepass LightLinker
	m_lightLinker.init(ExporterSettings::gSet.b_data, ExporterSettings::gSet.b_scene);
	m_lightLinker.prepass();
	m_lightLinker.setSceneSet(&m_exportedObjects);

	Node::m_lightLinker = &m_lightLinker;
	Node::m_scene_nodes = &m_exportedObjects;
}


void VRsceneExporter::exportObjectsPre()
{
	// Clear caches
	m_exportedObjects.clear();
	m_psys.clear();

	// Create particle system data
	// Needed for the correct first frame
	//
	if(ExporterSettings::gSet.m_isAnimation && ExporterSettings::gSet.IsFirstFrame())
		initDupli();
}


int VRsceneExporter::exportScene(const int &exportNodes, const int &exportGeometry)
{
	PRINT_INFO("VRsceneExporter::exportScene()");

	ExporterSettings::gSet.m_exportNodes  = exportNodes;
	ExporterSettings::gSet.m_exportMeshes = exportGeometry;

	double timeMeasure = 0.0;
	char   timeMeasureBuf[32];

	PRINT_INFO_EX("Exporting data for frame %i...", ExporterSettings::gSet.m_frameCurrent);
	timeMeasure = PIL_check_seconds_timer();

	Base *base = NULL;

	ExporterSettings::gSet.b_engine.update_progress(0.0f);

	PointerRNA sceneRNA;
	RNA_id_pointer_create((ID*)ExporterSettings::gSet.m_sce, &sceneRNA);
	BL::Scene bl_sce(sceneRNA);

	size_t nObjects = bl_sce.objects.length();

	float  expProgress = 0.0f;
	float  expProgStep = 1.0f / nObjects;
	int    progUpdateCnt = nObjects > 3000 ? 1000 : 100;
	if(nObjects > 3000) {
		progUpdateCnt = 1000;
	}
	else if(nObjects < 200) {
		progUpdateCnt = 10;
	}
	else {
		progUpdateCnt = 100;
	}

	exportObjectsPre();

	VRayNodeContext nodeCtx;
	VRayNodeExporter::exportVRayEnvironment(&nodeCtx);

	// Export stuff
	int exportInterrupt = false;

	base = (Base*)ExporterSettings::gSet.m_sce->base.first;
	nObjects = 0;
	while(base) {
		if(ExporterSettings::gSet.b_engine.test_break()) {
			ExporterSettings::gSet.b_engine.report(RPT_WARNING, "Export interrupted!");
			exportInterrupt = true;
			break;
		}

		Object *ob = base->object;
		base = base->next;

		// PRINT_INFO("Processing '%s'...", ob->id.name);

		// Skip object here, but not in dupli!
		// Dupli could be particles and it's better to
		// have animated 'visible' param there
		//
		if(ob->restrictflag & OB_RESTRICT_RENDER)
			continue;

		if(NOT(ob->lay & ExporterSettings::gSet.m_activeLayers))
			continue;

		if(m_skipObjects.count((void*)&ob->id)) {
			PRINT_INFO("Skipping object: %s", ob->id.name);
			continue;
		}

		exportObjectBase(ob);

		expProgress += expProgStep;
		nObjects++;
		if((nObjects % progUpdateCnt) == 0) {
			ExporterSettings::gSet.b_engine.update_progress(expProgress);
		}
	}

	if(NOT(exportInterrupt)) {
		// Export materials
		//
		BL::BlendData b_data(PointerRNA_NULL);
		BL::BlendData::materials_iterator maIt;

		if (ExporterSettings::gSet.b_engine.is_preview()) {
			RenderEngine *re = (RenderEngine*)ExporterSettings::gSet.b_engine.ptr.data;
			if(re->type->preview_main) {
				PointerRNA previewMainPtr;
				RNA_id_pointer_create((ID*)re->type->preview_main, &previewMainPtr);
				b_data = BL::BlendData(previewMainPtr);
			}
		}

		if(NOT(b_data))
			b_data = ExporterSettings::gSet.b_data;

		if(ExporterSettings::gSet.m_mtlOverride)
			VRayNodeExporter::exportMaterial(b_data, ExporterSettings::gSet.m_mtlOverride);

		// Export materials checking if we don't need to override it with global
		// override
		for(b_data.materials.begin(maIt); maIt != b_data.materials.end(); ++maIt) {
			BL::Material b_ma = *maIt;
			if(ExporterSettings::gSet.m_mtlOverride && Node::DoOverrideMaterial(b_ma))
				continue;
			VRayNodeExporter::exportMaterial(b_data, b_ma);
		}

		exportObjectsPost();
	}

	ExporterSettings::gSet.b_engine.update_progress(1.0f);

	BLI_timestr(PIL_check_seconds_timer()-timeMeasure, timeMeasureBuf, sizeof(timeMeasureBuf));

	exportClearCaches();

	if(exportInterrupt) {
		PRINT_INFO_EX("Exporting data for frame %i is interruped! [%s]",
					  ExporterSettings::gSet.m_frameCurrent, timeMeasureBuf);
		return 1;
	}

	PRINT_INFO_EX("Exporting data for frame %i done [%s]",
				  ExporterSettings::gSet.m_frameCurrent, timeMeasureBuf);

	return 0;
}


void VRsceneExporter::exportObjectsPost()
{
	// Export dupli/particle systems
	exportDupli();

	// Light linker settings only for the first frame
	if (ExporterSettings::gSet.IsFirstFrame())
		m_lightLinker.write(ExporterSettings::gSet.m_fileObject);
}


void VRsceneExporter::exportClearCaches()
{
	m_hideFromView.clear();

	// Clean plugin names cache
	VRayNodePluginExporter::clearNamesCache();

	// Clean Alt-D instances cache
	Node::FreeMeshCache();
}


void VRsceneExporter::exportObjectBase(Object *ob)
{
	if(NOT(GEOM_TYPE(ob) || EMPTY_TYPE(ob) || LIGHT_TYPE(ob)))
		return;

	PointerRNA objectRNA;
	RNA_id_pointer_create((ID*)ob, &objectRNA);
	BL::Object bl_ob(objectRNA);

	PointerRNA vrayObject = RNA_pointer_get(&bl_ob.ptr, "vray");

	PRINT_INFO("Processing object %s", ob->id.name);

	int data_updated = RNA_int_get(&vrayObject, "data_updated");
	if(data_updated)
		PRINT_INFO("Base object %s (update: %i)", ob->id.name, data_updated);

	if(bl_ob.is_duplicator()) {
		// If object is a dupli group holder and it's not animated -
		// export it only for the first frame
		//
		if(ExporterSettings::gSet.DoUpdateCheck()) {
			if(bl_ob.dupli_type() != BL::Object::dupli_type_NONE) {
				if(NOT(IsObjectUpdated((Object*)bl_ob.ptr.data) ||
					   IsObjectDataUpdated((Object*)bl_ob.ptr.data))) {
					return;
				}
			}
		}

		bl_ob.dupli_list_create(ExporterSettings::gSet.b_scene, 2);

		int overrideObjectID = RNA_int_get(&vrayObject, "dupliGroupIDOverride");
		int useInstancer     = RNA_boolean_get(&vrayObject, "use_instancer");

		const std::string &duplicatorName = GetIDName(bl_ob);

		BL::Object::dupli_list_iterator b_dup;
		for(bl_ob.dupli_list.begin(b_dup); b_dup != bl_ob.dupli_list.end(); ++b_dup) {
			if(ExporterSettings::gSet.b_engine && ExporterSettings::gSet.b_engine.test_break())
				break;

			BL::DupliObject bl_dupliOb      = *b_dup;
			BL::Object      bl_duplicatedOb =  bl_dupliOb.object();

			if(bl_dupliOb.hide() || bl_duplicatedOb.hide_render())
				continue;

			// Duplicated object could be duplicator itself
			// Check if we need to show it
			if(NOT(IsDuplicatorRenderable(bl_duplicatedOb)))
				continue;

			DupliObject *dupliOb = (DupliObject*)bl_dupliOb.ptr.data;

			if(NOT(GEOM_TYPE(dupliOb->ob) || LIGHT_TYPE(dupliOb->ob)))
				continue;

			MHash persistendID;
			MurmurHash3_x86_32((const void*)bl_dupliOb.persistent_id().data, 8 * sizeof(int), 42, &persistendID);

			const std::string &dupliNamePrefix = StripString("D" + BOOST_FORMAT_UINT(persistendID) + "@" + bl_ob.name());

			NodeAttrs dupliAttrs;
			dupliAttrs.override = true;
			// If dupli are shown via Instancer we need to hide
			// original object
			dupliAttrs.visible  = NOT(useInstancer);
			dupliAttrs.objectID = overrideObjectID;
			dupliAttrs.dupliHolder = bl_ob;

			if (NOT(useInstancer) || bl_duplicatedOb.type() == BL::Object::type_LAMP) {
				dupliAttrs.namePrefix = dupliNamePrefix;
				dupliAttrs.tm = bl_dupliOb.matrix();
			}

			if(bl_duplicatedOb.type() == BL::Object::type_LAMP) {
				exportObject(bl_duplicatedOb, dupliAttrs);
			}
			else {
				if(NOT(useInstancer)) {
					// If LightLinker contain duplicator,
					// we need to exclude it's objects
					//
					std::string pluginName = dupliAttrs.namePrefix + GetIDName((ID*)dupliOb->ob);
					m_lightLinker.excludePlugin(duplicatorName, pluginName);

					exportObject(bl_duplicatedOb, dupliAttrs);
				}
				else {
					std::string dupliBaseName;

					BL::ParticleSystem bl_psys = bl_dupliOb.particle_system();
					if(NOT(bl_psys))
						dupliBaseName = bl_ob.name();
					else {
						BL::ParticleSettings bl_pset = bl_psys.settings();
						dupliBaseName = bl_ob.name() + bl_psys.name() + bl_pset.name();
					}

					MyPartSystem *mySys = m_psys.get(dupliBaseName);
					MyParticle *myPa = new MyParticle();
					myPa->nodeName = GetIDName(&dupliOb->ob->id);
					myPa->particleId = persistendID;
					// Instancer use original object's transform,
					// so apply inverse matrix here.
					// When linking from file 'imat' is not valid,
					// so better to always calculate inverse matrix ourselves.
					//
					float duplicatedTmInv[4][4];
					copy_m4_m4(duplicatedTmInv, dupliOb->ob->obmat);
					invert_m4(duplicatedTmInv);
					float dupliTm[4][4];
					mul_m4_m4m4(dupliTm, dupliOb->mat, duplicatedTmInv);
					GetTransformHex(dupliTm, myPa->transform);

					mySys->append(myPa);

					// Set original object transform
					dupliAttrs.tm = bl_duplicatedOb.matrix_world();
					exportObject(bl_duplicatedOb, dupliAttrs);
				}
			}
		}

		bl_ob.dupli_list_clear();

		if(ob->transflag & OB_DUPLI) {
			// If dupli were not from particles (eg DupliGroup) skip base object
			if(NOT(ob->transflag & OB_DUPLIPARTS))
				return;
			else {
				// If there is fur we will check for "Render Emitter" later
				if(NOT(Node::HasHair(ob)))
					if(NOT(Node::DoRenderEmitter(ob)))
						return;
			}
		}
	}

	if(ExporterSettings::gSet.b_engine && ExporterSettings::gSet.b_engine.test_break())
		return;

	exportObject(bl_ob);

	// Reset update flag
	RNA_int_set(&vrayObject, "data_updated", CGR_NONE);
}


void VRsceneExporter::exportObject(BL::Object ob, const NodeAttrs &attrs)
{
	Object *b_ob = (Object*)ob.ptr.data;

	PointerRNA vrayObject = RNA_pointer_get(&ob.ptr, "vray");

	if (GEOM_TYPE(b_ob)) {
		// Smoke domain will be exported from Effects
		if (Node::IsSmokeDomain(b_ob))
			return;

		PointerRNA vrayClipper = RNA_pointer_get(&vrayObject, "VRayClipper");

		if (RNA_boolean_get(&vrayObject, "overrideWithScene")) {
			exportVRayAsset(ob, attrs);
		}
		else if (RNA_boolean_get(&vrayClipper, "enabled")) {
			exportVRayClipper(ob, attrs);
		}
		else {
			const std::string idName = attrs.namePrefix + GetIDName(ob);
			if(m_exportedObjects.count(idName))
				return;
			m_exportedObjects.insert(idName);

			BL::NodeTree ntree = VRayNodeExporter::getNodeTree(ExporterSettings::gSet.b_data, (ID*)b_ob);
			if(ntree)
				exportNodeFromNodeTree(ntree, b_ob, attrs);
			else
				exportNode(b_ob, attrs);
		}
	}
	else if (LIGHT_TYPE(b_ob)) {
		exportLamp(ob, attrs);
	}
}


void VRsceneExporter::exportNode(Object *ob, const NodeAttrs &attrs)
{
	PRINT_INFO("VRsceneExporter::exportNode(%s)",
			   ob->id.name);

	Node *node = new Node(ExporterSettings::gSet.m_sce, ExporterSettings::gSet.m_main, ob);
	node->setNamePrefix(attrs.namePrefix);
	if(attrs.override) {
		node->setTransform(attrs.tm);
		node->setVisiblity(attrs.visible);
		if(attrs.objectID > -1) {
			node->setObjectID(attrs.objectID);
		}
		if(attrs.dupliHolder.ptr.data) {
			node->setDupliHolder(attrs.dupliHolder);
		}
	}
	node->init(ExporterSettings::gSet.m_mtlOverrideName);
	node->initHash();

	// This will also check if object's mesh is valid
	if(NOT(node->preInitGeometry())) {
		delete node;
		return;
	}

	if(ExporterSettings::gSet.m_useHideFromView && m_hideFromView.hasData()) {
		RenderStats hideFromViewStats;
		hideFromViewStats.visibility             = !m_hideFromView.visibility.count(ob);
		hideFromViewStats.gi_visibility          = !m_hideFromView.gi_visibility.count(ob);

		hideFromViewStats.reflections_visibility = !m_hideFromView.reflections_visibility.count(ob);
		hideFromViewStats.refractions_visibility = !m_hideFromView.refractions_visibility.count(ob);
		hideFromViewStats.shadows_visibility     = !m_hideFromView.shadows_visibility.count(ob);
		hideFromViewStats.camera_visibility      = !m_hideFromView.camera_visibility.count(ob);

		node->setHideFromView(hideFromViewStats);
	}

	if(node->hasHair()) {
		node->writeHair();
		if(NOT(node->doRenderEmitter())) {
			delete node;
			return;
		}
	}

	if(ExporterSettings::gSet.m_exportMeshes) {
		int writeData = true;
		if (ExporterSettings::gSet.DoUpdateCheck())
			writeData = node->isObjectDataUpdated();
		if(writeData) {
			node->initGeometry();
			node->writeGeometry(ExporterSettings::gSet.m_fileGeom, ExporterSettings::gSet.m_frameCurrent);
		}
	}

	if(ExporterSettings::gSet.m_exportNodes) {
		int writeObject = true;
		if (ExporterSettings::gSet.DoUpdateCheck())
			writeObject = node->isObjectUpdated();
		int toDelete = false;
		if(writeObject) {
			toDelete = node->write(ExporterSettings::gSet.m_fileObject, ExporterSettings::gSet.m_frameCurrent);
		}
		else {
			if(m_hideFromView.hasData()) {
				node->writeHideFromView();
			}
		}
		if(toDelete) {
			delete node;
		}
	}
	else {
		delete node;
	}
}


void VRsceneExporter::exportNodeFromNodeTree(BL::NodeTree ntree, Object *ob, const NodeAttrs &attrs)
{
	PRINT_INFO("VRsceneExporter::exportNodeFromNodeTree(%s)",
			   ob->id.name);

	PointerRNA objectRNA;
	RNA_id_pointer_create((ID*)ob, &objectRNA);
	BL::Object bl_ob(objectRNA);

	// Export hair
	//
	Node::WriteHair(ob, attrs);

	if(NOT(Node::DoRenderEmitter(ob)))
		return;

	// Export object itself
	//
	BL::Node nodeOutput = VRayNodeExporter::getNodeByType(ntree, "VRayNodeObjectOutput");
	if(NOT(nodeOutput)) {
		PRINT_ERROR("Object: %s Node tree: %s => Output node not found!",
					ob->id.name, ntree.name().c_str());
		return;
	}

	BL::NodeSocket geometrySocket = VRayNodeExporter::getSocketByName(nodeOutput, "Geometry");
	if(NOT(geometrySocket && geometrySocket.is_linked())) {
		PRINT_ERROR("Object: %s Node tree: %s => Geometry node is not set!",
					ob->id.name, ntree.name().c_str());
		return;
	}

	const std::string pluginName = attrs.namePrefix + GetIDName(bl_ob, "OB");

	std::string transform = GetTransformHex(bl_ob.matrix_world());

	int visible  = true;
	int objectID = ob->index;

	// Prepare object context
	//
	VRayNodeContext nodeCtx;
	nodeCtx.obCtx.ob   = ob;
	nodeCtx.obCtx.sce  = ExporterSettings::gSet.m_sce;
	nodeCtx.obCtx.main = ExporterSettings::gSet.m_main;
	nodeCtx.obCtx.mtlOverrideName = ExporterSettings::gSet.m_mtlOverrideName;

	// Export object main properties
	//
	std::string geometry = VRayNodeExporter::exportSocket(ntree, geometrySocket, &nodeCtx);
	if(geometry == "NULL") {
		PRINT_ERROR("Object: %s Node tree: %s => Incorrect geometry!",
					ob->id.name, ntree.name().c_str());
		return;
	}

	BL::Node geometryNode = VRayNodeExporter::getConnectedNode(geometrySocket, &nodeCtx);
	if(geometryNode.bl_idname() == "VRayNodeLightMesh") {
		// No need to export Node - this object is LightMesh
		return;
	}

	BL::NodeSocket materialSocket = VRayNodeExporter::getSocketByName(nodeOutput, "Material");
	if(NOT(materialSocket && materialSocket.is_linked())) {
		PRINT_ERROR("Object: %s Node tree: %s => Material node is not set!",
					ob->id.name, ntree.name().c_str());
		return;
	}

	std::string material = VRayNodeExporter::exportSocket(ntree, materialSocket, &nodeCtx);
	if(material == "NULL") {
		PRINT_ERROR("Object: %s Node tree: %s => Incorrect material!",
					ob->id.name, ntree.name().c_str());
		return;
	}

	// Add MtlRenderStats and MtlWrapper from Object level for "one click" things
	//
	PointerRNA vrayObject = RNA_pointer_get(&bl_ob.ptr, "vray");

	material = Node::WriteMtlWrapper(&vrayObject, NULL, pluginName, material);
	material = Node::WriteMtlRenderStats(&vrayObject, NULL, pluginName, material);

	// Export 'MtlRenderStats' for "Hide From View"
	//
	if(ExporterSettings::gSet.m_useHideFromView && m_hideFromView.hasData()) {
		std::string hideFromViewName = "HideFromView@" + pluginName;

		AttributeValueMap hideFromViewAttrs;
		hideFromViewAttrs["base_mtl"] = material;
		hideFromViewAttrs["visibility"]             = BOOST_FORMAT_BOOL(!m_hideFromView.visibility.count(ob));
		hideFromViewAttrs["gi_visibility"]          = BOOST_FORMAT_BOOL(!m_hideFromView.gi_visibility.count(ob));
		hideFromViewAttrs["camera_visibility"]      = BOOST_FORMAT_BOOL(!m_hideFromView.camera_visibility.count(ob));
		hideFromViewAttrs["reflections_visibility"] = BOOST_FORMAT_BOOL(!m_hideFromView.reflections_visibility.count(ob));
		hideFromViewAttrs["refractions_visibility"] = BOOST_FORMAT_BOOL(!m_hideFromView.refractions_visibility.count(ob));
		hideFromViewAttrs["shadows_visibility"]     = BOOST_FORMAT_BOOL(!m_hideFromView.shadows_visibility.count(ob));

		// It's actually a material, but we will write it along with Node
		VRayNodePluginExporter::exportPlugin("NODE", "MtlRenderStats", hideFromViewName, hideFromViewAttrs);

		material = hideFromViewName;
	}

	// Check if we need to override some stuff;
	// comes from advanced DupliGroup export.
	//
	if(attrs.override) {
		PointerRNA vrayObject = RNA_pointer_get((PointerRNA*)&attrs.dupliHolder.ptr, "vray");

		visible  = attrs.visible;
		objectID = attrs.objectID;

		transform = GetTransformHex(attrs.tm);

		std::string overrideBaseName = pluginName + "@" + GetIDName((ID*)attrs.dupliHolder.ptr.data);
		material = Node::WriteMtlWrapper(&vrayObject, NULL, overrideBaseName, material);
		material = Node::WriteMtlRenderStats(&vrayObject, NULL, overrideBaseName, material);
	}

	PointerRNA vrayNode = RNA_pointer_get(&vrayObject, "Node");

	StrVector user_attributes;
	VRayNodeExporter::getUserAttributes(&vrayNode, user_attributes);

	AttributeValueMap pluginAttrs;
	pluginAttrs["material"]  = material;
	pluginAttrs["geometry"]  = geometry;
	pluginAttrs["objectID"]  = BOOST_FORMAT_INT(objectID);
	pluginAttrs["visible"]   = BOOST_FORMAT_INT(visible);
	pluginAttrs["transform"] = BOOST_FORMAT_TM(transform);

	if (user_attributes.size()) {
		pluginAttrs["user_attributes"] = BOOST_FORMAT_STRING(BOOST_FORMAT_LIST_JOIN_SEP(user_attributes, ";"));
	}

	VRayNodePluginExporter::exportPlugin("NODE", "Node", pluginName, pluginAttrs);
}


void VRsceneExporter::exportLamp(BL::Object ob, const NodeAttrs &attrs)
{
	BL::ID lampID = ob.data();
	if(NOT(lampID))
		return;

	BL::Lamp          lamp(lampID);
	PointerRNA        vrayLamp = RNA_pointer_get(&lamp.ptr, "vray");

	const std::string pluginName = attrs.namePrefix + GetIDName(ob, "LA");
	if(m_exportedObjects.count(pluginName))
		return;
	m_exportedObjects.insert(pluginName);

	// Find plugin ID
	std::string  pluginID;
	if(lamp.type() == BL::Lamp::type_AREA) {
		pluginID = "LightRectangle";
	}
	else if(lamp.type() == BL::Lamp::type_HEMI) {
		pluginID = "LightDome";
	}
	else if(lamp.type() == BL::Lamp::type_SPOT) {
		int spotType = RNA_enum_get(&vrayLamp, "spot_type");
		switch(spotType) {
			case 0: pluginID = "LightSpotMax"; break;
			case 1: pluginID = "LightIESMax";  break;
		}
	}
	else if(lamp.type() == BL::Lamp::type_POINT) {
		int omniType = RNA_enum_get(&vrayLamp, "omni_type");
		switch(omniType) {
			case 0: pluginID = "LightOmniMax";    break;
			case 1: pluginID = "LightAmbientMax"; break;
			case 2: pluginID = "LightSphere";     break;
		}
	}
	else if(lamp.type() == BL::Lamp::type_SUN) {
		int directType = RNA_enum_get(&vrayLamp, "direct_type");
		switch(directType) {
			case 0: pluginID = "LightDirectMax"; break;
			case 1: pluginID = "SunLight";       break;
		}
	}
	else {
		PRINT_ERROR("Lamp: %s Type: %i => Lamp type is not supported!",
					ob.name().c_str(), lamp.type());
		return;
	}

	AttributeValueMap pluginAttrs;
	PointerRNA propGroup = RNA_pointer_get(&vrayLamp, pluginID.c_str());

	// Get all non-mappable attribute values
	StrSet pluginAttrNames;
	VRayNodeExporter::getAttributesList(pluginID, pluginAttrNames, false);

	for(StrSet::const_iterator setIt = pluginAttrNames.begin(); setIt != pluginAttrNames.end(); ++setIt) {
		const std::string &attrName = *setIt;
		const std::string &attrValue = VRayNodeExporter::getValueFromPropGroup(&propGroup, (ID*)lamp.ptr.data, attrName.c_str());
		if(attrValue != "NULL")
			pluginAttrs[attrName] = attrValue;
	}

	// Now, get all mappable attribute values
	//
	StrSet socketAttrNames;
	VRayNodeExporter::getAttributesList(pluginID, socketAttrNames, true);

	BL::Node     lightNode(PointerRNA_NULL);
	BL::NodeTree lightTree = VRayNodeExporter::getNodeTree(ExporterSettings::gSet.b_data, (ID*)lamp.ptr.data);
	if(lightTree) {
		const std::string &vrayNodeType = boost::str(boost::format("VRayNode%s") % pluginID);

		lightNode = VRayNodeExporter::getNodeByType(lightTree, vrayNodeType);
		if(lightNode) {
			for(StrSet::const_iterator setIt = socketAttrNames.begin(); setIt != socketAttrNames.end(); ++setIt) {
				const std::string &attrName = *setIt;

				BL::NodeSocket sock = VRayNodeExporter::getSocketByAttr(lightNode, attrName);
				if(sock) {
					const std::string &attrValue = VRayNodeExporter::exportSocket(lightTree, sock);
					if(attrValue != "NULL")
						pluginAttrs[attrName] = attrValue;
				}
			}
		}
	}

	if(pluginID == "LightRectangle") {
		BL::AreaLamp  areaLamp(lamp);

		float sizeX = areaLamp.size() / 2.0f;
		float sizeY = areaLamp.shape() == BL::AreaLamp::shape_SQUARE ? sizeX : areaLamp.size_y() / 2.0f;

		pluginAttrs["u_size"] = BOOST_FORMAT_FLOAT(sizeX);
		pluginAttrs["v_size"] = BOOST_FORMAT_FLOAT(sizeY);

		pluginAttrs["use_rect_tex"] = BOOST_FORMAT_BOOL(pluginAttrs.count("rect_tex"));
	}
	else if(pluginID == "LightDome") {
		pluginAttrs["use_dome_tex"] = BOOST_FORMAT_BOOL(pluginAttrs.count("dome_tex"));
	}
	else if(pluginID == "LightSpotMax") {
		BL::SpotLamp spotLamp(lamp);

		pluginAttrs["fallsize"] = BOOST_FORMAT_FLOAT(spotLamp.spot_size());
	}
	else if(ELEM(pluginID, "LightRectangle", "LightSphere", "LightDome")) {
		pluginAttrs["objectID"] = BOOST_FORMAT_INT(ob.pass_index());
	}

	// Now, let's go through "Render Elements" and check if we have to
	// plug our light somewhere like "Light Select"
	//
	BL::NodeTree sceneTree = VRayNodeExporter::getNodeTree(ExporterSettings::gSet.b_data, (ID*)ExporterSettings::gSet.m_sce);
	if(sceneTree) {
		BL::Node chanNode = VRayNodeExporter::getNodeByType(sceneTree, "VRayNodeRenderChannels");
		if(chanNode) {
			StrSet  channels_raw;
			StrSet  channels_diffuse;
			StrSet  channels_specular;

			BL::Node::inputs_iterator inIt;
			for(chanNode.inputs.begin(inIt); inIt != chanNode.inputs.end(); ++inIt) {
				BL::NodeSocket chanSock = *inIt;
				if(chanSock && chanSock.is_linked() && chanSock.bl_idname() == "VRaySocketRenderChannel") {
					bool useChan = RNA_boolean_get(&chanSock.ptr, "use");
					if(useChan) {
						BL::Node chanNode = VRayNodeExporter::getConnectedNode(chanSock);
						if(chanNode && chanNode.bl_idname() == "VRayNodeRenderChannelLightSelect") {
							BL::NodeSocket lightsSock = VRayNodeExporter::getSocketByName(chanNode, "Lights");
							if(lightsSock) {
								BL::Node lightsConNode = VRayNodeExporter::getConnectedNode(lightsSock);
								if(lightsConNode && IS_OBJECT_SELECT_NODE(lightsConNode)) {
									ObList lampList;
									VRayNodeExporter::getNodeSelectObjects(lightsConNode, lampList);

									if(lampList.size()) {
										const int lightSelectType = RNA_enum_get(&chanNode.ptr, "type");
										const std::string &chanName = VRayNodeExporter::getPluginName(chanNode, sceneTree, NULL);

										ObList::const_iterator obIt;
										for(obIt = lampList.begin(); obIt != lampList.end(); ++obIt) {
											BL::Object lampOb = *obIt;
											if(lampOb.type() == BL::Object::type_LAMP && VRayNodeExporter::isObjectVisible(lampOb) && lampOb == ob) {
												switch(lightSelectType) {
													case  1: channels_diffuse.insert(chanName);  break;
													case  2: channels_specular.insert(chanName); break;
													default: channels_raw.insert(chanName);      break;
												}
											}
										}
									}
								}
							}
						}
					}
				}
			}

			if(channels_raw.size())
				pluginAttrs["channels_raw"]      = BOOST_FORMAT_LIST(channels_raw);
			if(channels_diffuse.size())
				pluginAttrs["channels_diffuse"]  = BOOST_FORMAT_LIST(channels_diffuse);
			if(channels_specular.size())
				pluginAttrs["channels_specular"] = BOOST_FORMAT_LIST(channels_specular);
		}
	}

	std::string transform = GetTransformHex(ob.matrix_world());
	if (attrs.override) {
		transform = GetTransformHex(attrs.tm);
	}

	pluginAttrs["transform"] = BOOST_FORMAT_TM(transform);

	VRayNodePluginExporter::exportPlugin("LIGHT", pluginID, pluginName, pluginAttrs);
}


void VRsceneExporter::exportVRayAsset(BL::Object ob, const NodeAttrs &attrs)
{
	PointerRNA vrayObject = RNA_pointer_get(&ob.ptr, "vray");
	PointerRNA vrayAsset  = RNA_pointer_get(&vrayObject, "VRayAsset");

	const std::string &pluginName = attrs.namePrefix + "Asset@" + GetIDName(ob);
	if(m_exportedObjects.count(pluginName))
		return;
	m_exportedObjects.insert(pluginName);

	std::string transform = GetTransformHex(ob.matrix_world());
	if (attrs.override) {
		transform = GetTransformHex(attrs.tm);
	}

	AttributeValueMap pluginAttrs;

	pluginAttrs["filepath"] = BOOST_FORMAT_STRING(BlenderUtils::GetFullFilepath(RNA_std_string_get(&vrayAsset, "sceneFilepath")).c_str());

	pluginAttrs["use_transform"] = BOOST_FORMAT_BOOL(RNA_boolean_get(&vrayAsset, "sceneUseTransform"));
	pluginAttrs["transform"]     = BOOST_FORMAT_TM(transform);

	pluginAttrs["add_nodes"]       = BOOST_FORMAT_BOOL(RNA_boolean_get(&vrayAsset, "sceneAddNodes"));
	pluginAttrs["add_lights"]      = BOOST_FORMAT_BOOL(RNA_boolean_get(&vrayAsset, "sceneAddLights"));

	// pluginAttrs["add_materials"]   = BOOST_FORMAT_BOOL(RNA_boolean_get(&vrayAsset, "sceneAddMaterials"));
	// pluginAttrs["add_cameras"]     = BOOST_FORMAT_BOOL(RNA_boolean_get(&vrayAsset, "sceneAddCameras"));
	// pluginAttrs["add_environment"] = BOOST_FORMAT_BOOL(RNA_boolean_get(&vrayAsset, "sceneAddEnvironment"));

	pluginAttrs["anim_type"]   = BOOST_FORMAT_INT(RNA_enum_ext_get(&vrayAsset, "anim_type"));
	pluginAttrs["anim_speed"]  = BOOST_FORMAT_FLOAT(RNA_float_get(&vrayAsset, "anim_speed"));
	pluginAttrs["anim_offset"] = BOOST_FORMAT_FLOAT(RNA_float_get(&vrayAsset, "anim_offset"));
	pluginAttrs["anim_start"]  = BOOST_FORMAT_INT(RNA_int_get(&vrayAsset, "anim_start"));
	pluginAttrs["anim_length"] = BOOST_FORMAT_INT(RNA_int_get(&vrayAsset, "anim_length"));

	if (ExporterSettings::gSet.m_mtlOverride) {
		pluginAttrs["material_override"] = ExporterSettings::gSet.m_mtlOverrideName;
	}

	if (RNA_boolean_get(&vrayAsset, "use_hide_objects")) {
		std::string hidden_objects = RNA_std_string_get(&vrayAsset, "hidden_objects");
		if (hidden_objects.size()) {
			StrVector hidden_objects_vec;
			boost::split(hidden_objects_vec, hidden_objects, boost::is_any_of(";"), boost::token_compress_on);

			StrSet hidden_objects_set;
			for (StrVector::const_iterator sIt = hidden_objects_vec.begin(); sIt != hidden_objects_vec.end(); ++sIt) {
				hidden_objects_set.insert(BOOST_FORMAT_STRING(*sIt));
			}

			pluginAttrs["hidden_objects"] = BOOST_FORMAT_LIST(hidden_objects_set);
		}
	}

	VRayNodePluginExporter::exportPlugin("NODE", "VRayScene", pluginName, pluginAttrs);
}


void VRsceneExporter::exportVRayClipper(BL::Object ob, const NodeAttrs &attrs)
{
	PointerRNA vrayObject  = RNA_pointer_get(&ob.ptr, "vray");
	PointerRNA vrayClipper = RNA_pointer_get(&vrayObject, "VRayClipper");

	const std::string &pluginName = attrs.namePrefix + "VRayClipper@" + GetIDName(ob);
	if(m_exportedObjects.count(pluginName))
		return;
	m_exportedObjects.insert(pluginName);

	char transform[CGR_TRANSFORM_HEX_SIZE];
	GetTransformHex(((Object*)ob.ptr.data)->obmat, transform);

	const std::string &material = VRayNodeExporter::exportMtlMulti(ExporterSettings::gSet.b_data, ob);

	AttributeValueMap pluginAttrs;
	pluginAttrs["enabled"] = "1";
	pluginAttrs["affect_light"]     = BOOST_FORMAT_BOOL(RNA_boolean_get(&vrayClipper, "affect_light"));
	pluginAttrs["only_camera_rays"] = BOOST_FORMAT_BOOL(RNA_boolean_get(&vrayClipper, "only_camera_rays"));
	pluginAttrs["clip_lights"]      = BOOST_FORMAT_BOOL(RNA_boolean_get(&vrayClipper, "clip_lights"));
	pluginAttrs["use_obj_mtl"]      = BOOST_FORMAT_BOOL(RNA_boolean_get(&vrayClipper, "use_obj_mtl"));
	pluginAttrs["set_material_id"]  = BOOST_FORMAT_BOOL(RNA_boolean_get(&vrayClipper, "set_material_id"));
	pluginAttrs["material_id"]      = BOOST_FORMAT_INT(RNA_int_get(&vrayClipper, "material_id"));
	pluginAttrs["object_id"]        = BOOST_FORMAT_INT(ob.pass_index());
	pluginAttrs["transform"]        = BOOST_FORMAT_TM(transform);

	const std::string &excludeGroupName = RNA_std_string_get(&vrayClipper, "exclusion_nodes");
	if (NOT(excludeGroupName.empty())) {
		StrSet exclusion_nodes;
		BL::BlendData::groups_iterator grIt;
		for (ExporterSettings::gSet.b_data.groups.begin(grIt); grIt != ExporterSettings::gSet.b_data.groups.end(); ++grIt) {
			BL::Group gr = *grIt;
			if (gr.name() == excludeGroupName) {
				BL::Group::objects_iterator grObIt;
				for (gr.objects.begin(grObIt); grObIt != gr.objects.end(); ++grObIt) {
					BL::Object ob = *grObIt;
					exclusion_nodes.insert(GetIDName(ob));
				}
				break;
			}
		}

		pluginAttrs["exclusion_mode"] = BOOST_FORMAT_INT(RNA_enum_get(&vrayClipper, "exclusion_mode"));
		pluginAttrs["exclusion_nodes"] = BOOST_FORMAT_LIST(exclusion_nodes);
	}

	if (NOT(material.empty()) && material != "NULL")
		pluginAttrs["material"] = material;

	VRayNodePluginExporter::exportPlugin("NODE", "VRayClipper", pluginName, pluginAttrs);
}


void VRsceneExporter::initDupli()
{
	PointerRNA sceneRNA;
	RNA_id_pointer_create((ID*)ExporterSettings::gSet.m_sce, &sceneRNA);
	BL::Scene bl_sce(sceneRNA);

	BL::Scene::objects_iterator bl_obIt;
	for(bl_sce.objects.begin(bl_obIt); bl_obIt != bl_sce.objects.end(); ++bl_obIt) {
		BL::Object bl_ob = *bl_obIt;
		if(bl_ob.type() == BL::Object::type_META)
			continue;
		if(bl_ob.is_duplicator()) {
			if(bl_ob.particle_systems.length()) {
				BL::Object::particle_systems_iterator bl_psysIt;
				for(bl_ob.particle_systems.begin(bl_psysIt); bl_psysIt != bl_ob.particle_systems.end(); ++bl_psysIt) {
					BL::ParticleSystem bl_psys = *bl_psysIt;
					BL::ParticleSettings bl_pset = bl_psys.settings();

					if (IS_PSYS_HAIR(bl_pset))
						continue;

					m_psys.get(bl_pset.name());
				}
			}
			if(bl_ob.dupli_type() != BL::Object::dupli_type_NONE)
				m_psys.get(bl_ob.name());
		}
	}
}


void VRsceneExporter::exportDupli()
{
	PyObject *out = ExporterSettings::gSet.m_fileObject;

	VRayExportable::initInterpolate(ExporterSettings::gSet.m_frameCurrent);

	for(MyPartSystems::const_iterator sysIt = m_psys.m_systems.begin(); sysIt != m_psys.m_systems.end(); ++sysIt) {
		const std::string   psysName = sysIt->first;
		const MyPartSystem *parts    = sysIt->second;

		PYTHON_PRINTF(out, "\nInstancer Dulpi%s {", StripString(psysName).c_str());
		PYTHON_PRINTF(out, "\n\tinstances=%sList(%i", VRayExportable::m_interpStart, ExporterSettings::gSet.m_isAnimation ? ExporterSettings::gSet.m_frameCurrent : 0);
		if(parts->size()) {
			PYTHON_PRINT(out, ",");
			for(Particles::const_iterator paIt = parts->m_particles.begin(); paIt != parts->m_particles.end(); ++paIt) {
				const MyParticle *pa = *paIt;

				PYTHON_PRINTF(out, "List(%u,TransformHex(\"%s\"),TransformHex(\"%s\"),%s)", pa->particleId, pa->transform, pa->velocity, pa->nodeName.c_str());

				if(paIt != --parts->m_particles.end()) {
					PYTHON_PRINT(out, ",");
				}
			}
		}
		PYTHON_PRINTF(out, ")%s;", VRayExportable::m_interpEnd);
		PYTHON_PRINTF(ExporterSettings::gSet.m_fileObject, "\n}\n");
	}

	m_psys.clear();
}