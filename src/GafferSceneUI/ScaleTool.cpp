//////////////////////////////////////////////////////////////////////////
//
//  Copyright (c) 2016, John Haddon. All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions are
//  met:
//
//      * Redistributions of source code must retain the above
//        copyright notice, this list of conditions and the following
//        disclaimer.
//
//      * Redistributions in binary form must reproduce the above
//        copyright notice, this list of conditions and the following
//        disclaimer in the documentation and/or other materials provided with
//        the distribution.
//
//      * Neither the name of John Haddon nor the names of
//        any other contributors to this software may be used to endorse or
//        promote products derived from this software without specific prior
//        written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
//  IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
//  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
//  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
//  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
//  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
//  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
//  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
//  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
//  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
//  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//////////////////////////////////////////////////////////////////////////

#include "GafferSceneUI/ScaleTool.h"

#include "GafferSceneUI/SceneView.h"

#include "GafferUI/ScaleHandle.h"

#include "Gaffer/MetadataAlgo.h"
#include "Gaffer/ScriptNode.h"
#include "Gaffer/UndoScope.h"

#include "OpenEXR/ImathMatrixAlgo.h"

#include "boost/bind.hpp"

using namespace std;
using namespace Imath;
using namespace IECore;
using namespace Gaffer;
using namespace GafferUI;
using namespace GafferScene;
using namespace GafferSceneUI;

//////////////////////////////////////////////////////////////////////////
// Internal utilities
//////////////////////////////////////////////////////////////////////////

namespace
{

M44f signOnlyScaling( const M44f &m )
{
	V3f scale;
	V3f shear;
	V3f rotate;
	V3f translate;

	extractSHRT( m, scale, shear, rotate, translate );

	M44f result;

	result.translate( translate );
	result.rotate( rotate );
	result.shear( shear );
	result.scale( V3f( sign( scale.x ), sign( scale.y ), sign( scale.z ) ) );

	return result;
}

} // namespace

//////////////////////////////////////////////////////////////////////////
// ScaleTool
//////////////////////////////////////////////////////////////////////////

IE_CORE_DEFINERUNTIMETYPED( ScaleTool );

ScaleTool::ToolDescription<ScaleTool, SceneView> ScaleTool::g_toolDescription;

ScaleTool::ScaleTool( SceneView *view, const std::string &name )
	:	TransformTool( view, name )
{
	static Style::Axes axes[] = { Style::X, Style::Y, Style::Z, Style::XY, Style::XZ, Style::YZ, Style::XYZ };
	static const char *handleNames[] = { "x", "y", "z", "xy", "xz", "yz", "xyz" };

	for( int i = 0; i < 7; ++i )
	{
		ScaleHandlePtr handle = new ScaleHandle( axes[i] );
		handle->setRasterScale( 75 );
		handles()->setChild( handleNames[i], handle );
		// connect with group 0, so we get called before the Handle's slot does.
		handle->dragBeginSignal().connect( 0, boost::bind( &ScaleTool::dragBegin, this, axes[i] ) );
		handle->dragMoveSignal().connect( boost::bind( &ScaleTool::dragMove, this, ::_1, ::_2 ) );
		handle->dragEndSignal().connect( boost::bind( &ScaleTool::dragEnd, this ) );
	}
}

ScaleTool::~ScaleTool()
{
}

bool ScaleTool::affectsHandles( const Gaffer::Plug *input ) const
{
	if( TransformTool::affectsHandles( input ) )
	{
		return true;
	}

	return input == scenePlug()->transformPlug();
}

void ScaleTool::updateHandles( float rasterScale )
{
	const Selection &selection = this->selection();

	M44f pivotMatrix;
	{
		Context::Scope upstreamScope( selection.upstreamContext.get() );
		const V3f pivot = selection.transformPlug->pivotPlug()->getValue();
		pivotMatrix.translate( pivot );
	}

	M44f handlesMatrix = pivotMatrix * selection.transformPlug->matrix() * selection.sceneToTransformSpace().inverse();
	// We want to take the sign of the scaling into account so that
	// our handles point in the right direction. But we don't want
	// the magnitude because a non-uniform handle scale breaks the
	// operation of the xy/xz/yz handles.
	handlesMatrix = signOnlyScaling( handlesMatrix );

	handles()->setTransform(
		handlesMatrix
	);

	Scale scale( this );
	for( ScaleHandleIterator it( handles() ); !it.done(); ++it )
	{
		(*it)->setEnabled( scale.canApply( (*it)->axisMask() ) );
		(*it)->setRasterScale( rasterScale );
	}
}

void ScaleTool::scale( const Imath::V3f &scale )
{
	Scale( this ).apply( scale );
}

IECore::RunTimeTypedPtr ScaleTool::dragBegin( GafferUI::Style::Axes axes )
{
	m_drag = Scale( this );
	TransformTool::dragBegin();
	return nullptr; // Let the handle start the drag.
}

bool ScaleTool::dragMove( const GafferUI::Gadget *gadget, const GafferUI::DragDropEvent &event )
{
	UndoScope undoScope( selection().transformPlug->ancestor<ScriptNode>(), UndoScope::Enabled, undoMergeGroup() );
	m_drag.apply( static_cast<const ScaleHandle *>( gadget )->scaling( event ) );
	return true;
}

bool ScaleTool::dragEnd()
{
	TransformTool::dragEnd();
	return false;
}

//////////////////////////////////////////////////////////////////////////
// ScaleTool::Scale
//////////////////////////////////////////////////////////////////////////

ScaleTool::Scale::Scale( const ScaleTool *tool )
{
	Context::Scope scopedContext( tool->view()->getContext() );
	m_plug = tool->selection().transformPlug->scalePlug();
	m_originalScale = m_plug->getValue();
	m_time = tool->view()->getContext()->getTime();
}

bool ScaleTool::Scale::canApply( const Imath::V3i &axisMask ) const
{
	for( int i = 0; i < 3; ++i )
	{
		if( axisMask[i] && !canSetValueOrAddKey( m_plug->getChild( i ) ) )
		{
			return false;
		}
	}

	return true;
}

void ScaleTool::Scale::apply( const Imath::V3f &scale ) const
{
	for( int i = 0; i < 3; ++i )
	{
		FloatPlug *plug = m_plug->getChild( i );
		if( canSetValueOrAddKey( plug ) )
		{
			setValueOrAddKey( plug, m_time, m_originalScale[i] * scale[i] );
		}
	}
}
