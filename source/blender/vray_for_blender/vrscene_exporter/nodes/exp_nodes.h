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

#ifndef CGR_EXPORT_NODES_H
#define CGR_EXPORT_NODES_H

#include <Python.h>

#include <algorithm>

#include <boost/format.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/join.hpp>

#include "exp_scene.h"

#include "CGR_string.h"
#include "CGR_rna.h"

#include <BLI_string.h>


#define SKIP_TYPE(attrType) (\
	attrType == "LIST"              || \
	attrType == "INT_LIST"          || \
	attrType == "FLOAT_LIST"        || \
	attrType == "MATRIX"            || \
	attrType == "TRANSFORM"         || \
	attrType == "TRANSFORM_TEXTURE")

#define OUTPUT_TYPE(attrType) (\
	attrType == "OUTPUT_PLUGIN"            || \
	attrType == "OUTPUT_COLOR"             || \
	attrType == "OUTPUT_FLOAT_TEXTURE"     || \
	attrType == "OUTPUT_VECTOR_TEXTURE"    || \
	attrType == "OUTPUT_TRANSFORM_TEXTURE" || \
	attrType == "OUTPUT_TEXTURE")

#define MAPPABLE_TYPE(attrType) (\
	attrType == "BRDF"          || \
	attrType == "MATERIAL"      || \
	attrType == "GEOMETRY"      || \
	attrType == "PLUGIN"        || \
	attrType == "TEXTURE"       || \
	attrType == "FLOAT_TEXTURE" || \
	attrType == "INT_TEXTURE"   || \
	attrType == "VECTOR"        || \
	attrType == "UVWGEN")

#define NOT_ANIMATABLE_TYPE(attrType) (\
	attrType == "BRDF"     || \
	attrType == "MATERIAL" || \
	attrType == "GEOMETRY" || \
	attrType == "PLUGIN")

#define BOOST_FORMAT_STRING(s) boost::str(boost::format("\"%s\"") % s);
#define BOOST_FORMAT_FLOAT(f)  boost::str(boost::format("%.6f") % f)
#define BOOST_FORMAT_TM(tm)    boost::str(boost::format("TransformHex(\"%s\")") % tm)
#define BOOST_FORMAT_INT(i)    boost::str(boost::format("%i") % i)
#define BOOST_FORMAT_BOOL(i)   BOOST_FORMAT_INT(i)

#define BOOST_FORMAT_COLOR(c)   boost::str(boost::format("Color(%.6f,%.6f,%.6f)")       % c[0] % c[1] % c[2]);
#define BOOST_FORMAT_ACOLOR(c)  boost::str(boost::format("AColor(%.6f,%.6f,%.6f,%.6f)") % c[0] % c[1] % c[2] % c[3]);
#define BOOST_FORMAT_ACOLOR3(c) boost::str(boost::format("AColor(%.6f,%.6f,%.6f,1.0)")  % c[0] % c[1] % c[2]);
#define BOOST_FORMAT_VECTOR(v)  boost::str(boost::format("Vector(%.6f,%.6f,%.6f)")      % v[0] % v[1] % v[2])


namespace VRayScene {

struct AttrValue {
	int          frame;
	std::string  value;
	MHash        hash;
};

typedef std::map<std::string, AttrValue>  AttrCache;
typedef std::map<std::string, AttrCache>  PluginCache;


class VRayNodeCache {
public:
	bool pluginInCache(const std::string &pluginName) {
		return !!(m_pluginCache.find(pluginName) != m_pluginCache.end());
	}

	void addToCache(const std::string &pluginName, const std::string &attrName, const int &frame, const std::string &attrValue, const MHash &hash) {
		m_pluginCache[pluginName][attrName].frame = frame;
		m_pluginCache[pluginName][attrName].value = attrValue;
		m_pluginCache[pluginName][attrName].hash  = hash;
	}

	const int getCachedFrame(const std::string &pluginName, const std::string &attrName) {
		return m_pluginCache[pluginName][attrName].frame;
	}

	const std::string getCachedValue(const std::string &pluginName, const std::string &attrName) {
		return m_pluginCache[pluginName][attrName].value;
	}

	const MHash getCachedHash(const std::string &pluginName, const std::string &attrName) {
		return m_pluginCache[pluginName][attrName].hash;
	}

	void clearCache() {
		m_pluginCache.clear();
	}

	void showCacheContents() {
		PluginCache::const_iterator cacheIt;
		for(cacheIt = m_pluginCache.begin(); cacheIt != m_pluginCache.end(); ++cacheIt) {
			const std::string  pluginName = cacheIt->first;
			const AttrCache   &attrCache  = cacheIt->second;

			std::cout << pluginName << std::endl;

			AttrCache::const_iterator attrIt;
			for(attrIt = attrCache.begin(); attrIt != attrCache.end(); ++attrIt) {
				const std::string attrName  = attrIt->first;

				const int         attrFrame = attrIt->second.frame;
				const std::string attrValue = attrIt->second.value;

				std::cout << "  " << attrName << " = " << attrValue << " [" << attrFrame << "]" << std::endl;
			}
		}
	}

private:
	PluginCache  m_pluginCache;

};


struct VRayObjectContext {
	Scene       *sce;
	Main        *main;
	Object      *ob;

	std::string  mtlOverride;
};


struct VRayNodeContext {
	VRayNodeContext(BL::NodeTree nt, BL::Node n):ntree(nt),node(n) {}

	BL::NodeTree ntree;
	BL::Node     node;
};


class VRayNodeExporter {
public:
	static void             getAttributesList(const std::string &pluginID, StrSet &attrSet, bool mappable=false);

	static std::string      getValueFromPropGroup(PointerRNA *propGroup, ID *holder, const std::string &attrName);

	static BL::NodeTree     getNodeTree(BL::BlendData b_data, ID *id);

	static BL::Node         getNodeByType(BL::NodeTree nodeTree, const std::string &nodeType);

	static BL::NodeSocket   getSocketByName(BL::Node node, const std::string &socketName);
	static BL::NodeSocket   getSocketByAttr(BL::Node node, const std::string &attrName);

	static BL::Node         getConnectedNode(BL::NodeTree nodeTree, BL::NodeSocket socket);
	static BL::Node         getConnectedNode(BL::NodeTree nodeTree, BL::Node node, const std::string &socketName);
	static BL::NodeSocket   getConnectedSocket(BL::NodeTree nodeTree, BL::NodeSocket socket);

	static std::string      exportVRayNode(BL::NodeTree ntree, BL::Node node, VRayObjectContext *context=NULL, const AttributeValueMap &manualAttrs=AttributeValueMap());
	static std::string      exportVRayNodeAttributes(BL::NodeTree ntree, BL::Node node, VRayObjectContext *context=NULL, const AttributeValueMap &manualAttrs=AttributeValueMap());

	static std::string      exportSocket(BL::NodeTree ntree, BL::NodeSocket socket, VRayObjectContext *context=NULL);
	static std::string      exportSocket(BL::NodeTree ntree, BL::Node node, const std::string &socketName, VRayObjectContext *context=NULL);

	static ExpoterSettings *m_set;

private:
	static std::string      exportLinkedSocket(BL::NodeTree ntree, BL::NodeSocket socket, VRayObjectContext *context=NULL);
	static std::string      exportDefaultSocket(BL::NodeTree ntree, BL::NodeSocket socket);

	static std::string      exportVRayNodeBlenderOutputMaterial(BL::NodeTree ntree, BL::Node node, VRayObjectContext *context);
	static std::string      exportVRayNodeBlenderOutputGeometry(BL::NodeTree ntree, BL::Node node, VRayObjectContext *context);

	static std::string      exportVRayNodeLightMesh(BL::NodeTree ntree, BL::Node node, VRayObjectContext *context);
	static std::string      exportVRayNodeGeomDisplacedMesh(BL::NodeTree ntree, BL::Node node, VRayObjectContext *context);

	static std::string      exportVRayNodeSelectObject(BL::NodeTree ntree, BL::Node node);
	static std::string      exportVRayNodeSelectGroup(BL::NodeTree ntree, BL::Node node);
	static std::string      exportVRayNodeSelectNodeTree(BL::NodeTree ntree, BL::Node node);

	static std::string      exportVRayNodeBRDFLayered(BL::NodeTree ntree, BL::Node node);
	static std::string      exportVRayNodeTexLayered(BL::NodeTree ntree, BL::Node node);
	static std::string      exportVRayNodeBitmapBuffer(BL::NodeTree ntree, BL::Node node);
	static std::string      exportVRayNodeTexGradRamp(BL::NodeTree ntree, BL::Node node);
	static std::string      exportVRayNodeTexRemap(BL::NodeTree ntree, BL::Node node);

	static BL::Texture      getTextureFromIDRef(PointerRNA *ptr, const std::string &propName);

};


class VRayNodePluginExporter {
public:
	static int   exportPlugin(const std::string &pluginType, const std::string &pluginID, const std::string &pluginName, const AttributeValueMap &pluginAttrs);
	static void  clearNamesCache();
	static void  clearNodesCache();

private:
	static VRayNodeCache    m_nodeCache;
	static StrSet           m_namesCache;

};

} // namespace VRayScene

#endif // CGR_EXPORT_NODES_H