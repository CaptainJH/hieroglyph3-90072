//--------------------------------------------------------------------------------
// This file is a portion of the Hieroglyph 3 Rendering Engine.  It is distributed
// under the MIT License, available in the root of this distribution and 
// at the following URL:
//
// http://www.opensource.org/licenses/mit-license.php
//
// Copyright (c) Jason Zink 
//--------------------------------------------------------------------------------

//--------------------------------------------------------------------------------
#include "GlyphRift/ViewRift.h"
#include "Entity3D.h"
#include "Scene.h"
#include "Texture2dConfigDX11.h"
#include "Log.h"
#include "IParameterManager.h"
#include "PipelineManagerDX11.h"
#include "Texture2dDX11.h"
#include "BoundsVisualizerActor.h"
#include "SceneGraph.h"
//--------------------------------------------------------------------------------
using namespace Glyph3;
//--------------------------------------------------------------------------------
ViewRift::ViewRift( RiftHMDPtr hmd, ResourcePtr RenderTarget, int SwapChain  )
{
	m_pHmd = hmd;

	// Get the desired texture sizes for the eye render targets.  Note that these 
	// are typically larger than the resolution of the HMD's display panel itself.

	unsigned int width = m_pHmd->DesiredEyeTextureWidth();
	unsigned int height = m_pHmd->DesiredEyeTextureHeight();

	// Create a single depth buffer, which will be used in rendering the scene
	// into each eye.

	Texture2dConfigDX11 depthConfig;
	depthConfig.SetDepthBuffer( width, height );
	m_DepthTarget = RendererDX11::Get()->CreateTexture2D( &depthConfig, 0 );

	// Create two render targets - one for each eye.  These are created with both
	// render target and shader resource bind points so that they can be used by
	// the Oculus SDK to do the distortion rendering at the end of the frame.

	Texture2dConfigDX11 renderConfig;
	renderConfig.SetWidth( width );
	renderConfig.SetHeight( height );
	renderConfig.SetFormat( DXGI_FORMAT_R8G8B8A8_UNORM );
	renderConfig.SetBindFlags( D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE );
	m_EyeTargets[0] = RendererDX11::Get()->CreateTexture2D( &renderConfig, 0 );
	m_EyeTargets[1] = RendererDX11::Get()->CreateTexture2D( &renderConfig, 0 );

	// Read back the actual size of the texture created for use in the other
	// setup functions below.

	D3D11_TEXTURE2D_DESC desc = RendererDX11::Get()->GetTexture2DByIndex( m_DepthTarget->m_iResource )->GetActualDescription();

	// Create a view port to use on the scene.  Since they are the exact same
	// for both eyes, we can just use one viewport.

	D3D11_VIEWPORT viewport;
	viewport.Width = static_cast< float >( desc.Width );
	viewport.Height = static_cast< float >( desc.Height );
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;

	m_iViewports[0] = RendererDX11::Get()->CreateViewPort( viewport );


	// Set up the D3D texture data, including ID3D11Texture2D and 
	// ID3D11ShaderResourceView pointers (which are both accessible through the
	// passed in ResourcePtr object).  These will be used in distortion
	// rendering.

	m_pHmd->ConfigureEyeTexture( 0, m_EyeTargets[0] ); 
	m_pHmd->ConfigureEyeTexture( 1, m_EyeTargets[1] );

	// Pass the swap chain and render target to the Oculus HMD device.  This
	// is used by the Oculus SDK to map the distorted eye textures to the 
	// backbuffer and to synchronize GPU timing.

	m_pHmd->ConfigureRendering( RenderTarget, SwapChain );


	// Set up the projection matrices according to the FOV for each eye.  These
	// can be calculated once and then reused as long as the FOV is not changed
	// during runtime.

	m_proj[0] = m_pHmd->GetPerspectiveFov( 0, 0.01f, 10000.0f );
	m_proj[1] = m_pHmd->GetPerspectiveFov( 1, 0.01f, 10000.0f );
}
//--------------------------------------------------------------------------------
ViewRift::~ViewRift()
{
}
//--------------------------------------------------------------------------------
void ViewRift::Update( float fTime )
{
}
//--------------------------------------------------------------------------------
void ViewRift::QueuePreTasks( RendererDX11* pRenderer )
{
	if ( m_pEntity != NULL )
	{
		Matrix4f view = m_pEntity->Transform.GetView();
		SetViewMatrix( view );
	}

	// Queue this view into the renderer for processing.
	pRenderer->QueueTask( this );

	if ( m_pScene )
	{
		// Run through the graph and pre-render the entities
		m_pScene->PreRender( pRenderer, VT_PERSPECTIVE );
	}
}
//--------------------------------------------------------------------------------
void ViewRift::ExecuteTask( PipelineManagerDX11* pPipelineManager, IParameterManager* pParamManager )
{
	// Here we assume that the ovrHmd_BeginFrame function has already been called
	// by the application at the beginning of the rendering phase.  The 
	// application will also call the ovrHmd_EndFrame function after all rendering
	// is completed.

	if ( m_pScene )
	{
		// Configure the desired viewports in this pipeline.  This state is the 
		// same for both eyes, so we pull it out of the loop.
		ConfigureViewports( pPipelineManager );

		for ( unsigned int eye = 0; eye < 2; ++eye )
		{
			// Set up the view matrices for this eye.  This starts by getting the 
			// view matrix from the node that this view's entity is attached to.  
			// That represents the camera's node (instead of its body).  We then apply
			// the eye's spatial state (including position and orientation) to that
			// parent location.
			//
			// NOTE: This view's entity location and orientation is updated according
			//       to the Rift's *head* location.  That provides an approximate location
			//       for using to visualize the location of the rift in world space.
			//       This is why we need to grab the parent's view - the eye matrices are
			//       in relation to the whole rift, as opposed to relative to the head.

			Matrix4f centerView = m_pEntity->GetParent()->Transform.GetView();	
			m_view[eye] = centerView * m_pHmd->GetEyeSpatialState( eye );

			// Set the parameters for rendering this view
			pPipelineManager->ClearRenderTargets();
			pPipelineManager->OutputMergerStage.DesiredState.RenderTargetViews.SetState( 0, m_EyeTargets[eye]->m_iResourceRTV );
			pPipelineManager->OutputMergerStage.DesiredState.DepthTargetViews.SetState( m_DepthTarget->m_iResourceDSV );
			pPipelineManager->ApplyRenderTargets();
			pPipelineManager->ClearBuffers( m_vColor, 1.0f );

			// Set this view's render parameters
			SetViewMatrix( m_view[eye] );
			SetProjMatrix( m_proj[eye] );
			SetRenderParams( pParamManager );

			// Set the light parameters (currently only supporting the first light...)
			if ( m_pScene->GetLightCount() > 0 ) {
				m_pScene->GetLight( 0 )->Parameters.SetRenderParams( pParamManager );
			}

			// Run through the graph and render each of the entities
			m_pScene->GetRoot()->Render( pPipelineManager, pParamManager, VT_PERSPECTIVE );

			// If the debug view is enabled, then we can render some additional scene
			// related information as an overlay on this view.  Note that this is
			// accomplished without the actor being attached to the scene!
			if ( m_bDebugViewEnabled )
			{
				std::vector<Entity3D*> list;
				GetAllEntities( m_pScene->GetRoot(), list );
				m_pDebugVisualizer->UpdateBoundsData( list );

				m_pDebugVisualizer->GetBody()->Render( pPipelineManager, pParamManager, VT_PERSPECTIVE );
			}
		}
	}
}
//--------------------------------------------------------------------------------
void ViewRift::Resize( UINT width, UINT height )
{
	RendererDX11::Get()->ResizeTexture( m_DepthTarget, width, height );
	RendererDX11::Get()->ResizeViewport( m_iViewports[0], width, height );
}
//--------------------------------------------------------------------------------
std::wstring ViewRift::GetName()
{
	return( L"ViewRift" );
}
//--------------------------------------------------------------------------------