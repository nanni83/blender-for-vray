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

#include "exp_nodes.h"


std::string VRayNodeExporter::exportVRayNodeLightMesh(BL::NodeTree ntree, BL::Node node, BL::NodeSocket fromSocket, VRayNodeContext *context)
{
	if(NOT(context->obCtx.ob)) {
		PRINT_ERROR("Node tree: %s => Node name: %s => Incorrect node context! Probably used in not suitable node tree type.",
					ntree.name().c_str(), node.name().c_str());
		return "NULL";
	}

	BL::NodeSocket geomSock = VRayNodeExporter::getSocketByName(node, "Geometry");
	if(NOT(geomSock.is_linked())) {
		PRINT_ERROR("Node tree: %s => Node name: %s => Geometry socket is not linked!",
					ntree.name().c_str(), node.name().c_str());

		return "NULL";
	}

	char transform[CGR_TRANSFORM_HEX_SIZE];
	GetTransformHex(context->obCtx.ob->obmat, transform);

	AttributeValueMap manualAttrs;
	manualAttrs["geometry"]  = VRayNodeExporter::exportLinkedSocket(ntree, geomSock, context);
	manualAttrs["transform"] = BOOST_FORMAT_TM(transform);

	return VRayNodeExporter::exportVRayNodeAttributes(ntree, node, fromSocket, context, manualAttrs);
}