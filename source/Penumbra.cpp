#include "Penumbra.h"

#include "Controls.h"
#include "Diag.h"
#include "GLState.h"
#include "Names.h"
#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace penumbra;

//---------------------------------------------------------------------------
// The eighth argument is the plugin TYPE, and it is the only thing in the
// whole repo that makes this a mixer rather than an effect. The input count
// is a SEPARATE declaration, in the constructor. See genlock's AGENTS.md.
//---------------------------------------------------------------------------
static CFFGLPluginInfo PluginInfo(
	PluginFactory< Penumbra >,// Create method
	names::kMixerId,          // Plugin unique ID of maximum length 4.
	names::kMixerName,        // Plugin name
	2,                        // API major version number
	1,                        // API minor version number
	0,                        // Plugin major version number
	1,                        // Plugin minor version number
	FF_MIXER,                 // Plugin type
	"Coloured shadows. Three coloured lamps light a white screen with the layer below printed on it, and this layer stands between them and the screen: its alpha (or its luma) is the occluder, a person's mask or any clip. Each point of the screen shows the sum of the lamps it can see, so red, green and blue lamps throw cyan, magenta and yellow shadows, black only where all three are blocked. Distance moves the occluder towards the lamps: the shadows part and their edges soften, by similar triangles. The layer's opacity is the occluder's.\n\nThis is a MIXER: set it as the occluder layer's blend mode, over the clip.",
	"Penumbra FFGL mixer" );

static_assert( Penumbra::PT_COUNT - Penumbra::PT_ABOUT_FIRST == stoatworks::about::kParamCount,
               "the About block's size changed with the generated header" );
static_assert( Penumbra::PT_LAMP_LAST + 1 == Penumbra::PT_SCREEN, "the lamps' blocks overlap the Screen group" );

namespace
{
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

std::string sizeText( const FFGLTextureStruct& t )
{
	return std::to_string( t.Width ) + "x" + std::to_string( t.Height ) + " of " + std::to_string( t.HardwareWidth ) + "x"
	       + std::to_string( t.HardwareHeight );
}

/// How many Opacity changes the log records, per instance. Enough to follow a
/// whole layer transition frame by frame (a 2 s fade at 60 fps is 120); not a
/// line a frame for the length of a show.
constexpr int kOpacityLines = 400;

/// Instances are numbered in creation order, so a host session's log can tell
/// the layer's mixer from a transition's (Arena makes separate instances).
int gInstances = 0;

bool compile( FFGLShader& shader, const std::string& fragment, const char* what )
{
	if( shader.Compile( AssembleVertex(), fragment ) )
		return true;
	//Returning FF_FAIL from InitGL is invisible to the operator: the mixer
	//simply does nothing in Resolume. These lines are the only record.
	diag::error( std::string( "the " ) + what + " shader failed to compile - the mixer will do nothing" );
	FFGLLog::LogToHost( ( std::string( "Penumbra: " ) + what + " shader failed to compile" ).c_str() );
	return false;
}

/// One pass into one half of the table: its framebuffer, its whole extent.
/// ScopedFBOBinding is not used because it does not restore the viewport,
/// and the host's framebuffer and viewport are put back by hand before the
/// screen pass.
void drawInto( const TableBuffer& target, FFGLScreenQuad& quadToDraw )
{
	glBindFramebuffer( GL_FRAMEBUFFER, target.Framebuffer() );
	glViewport( 0, 0, target.Width(), target.Height() );
	quadToDraw.Draw();
}
} // namespace

Penumbra::Penumbra()
{
	//A mixer takes exactly two. This is a separate declaration from the type
	//above, read by the host through different function codes entirely.
	SetMinInputs( 2 );
	SetMaxInputs( 2 );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//---------------------------------------------------------------------
	params[ PT_LINEAR_LIGHT ] = 1.0f;
	params[ PT_ROOM_LIGHT ]   = ParamForRoom( 0.0 );
	params[ PT_OPACITY ]      = 1.0f;
	params[ PT_OCCLUDER ]     = static_cast< float >( OccluderKind::Alpha );
	params[ PT_DISTANCE ]     = ParamForDistance( kDistanceDefault );
	for( int i = 0; i < kLamps; ++i )
	{
		params[ LampParam( i, LAMP_RED ) ]   = static_cast< float >( kLampDefaults[ i ].r );
		params[ LampParam( i, LAMP_GREEN ) ] = static_cast< float >( kLampDefaults[ i ].g );
		params[ LampParam( i, LAMP_BLUE ) ]  = static_cast< float >( kLampDefaults[ i ].b );
		params[ LampParam( i, LAMP_X ) ]     = ParamForLampX( kLampDefaults[ i ].x );
		params[ LampParam( i, LAMP_Y ) ]     = ParamForLampY( kLampDefaults[ i ].y );
		params[ LampParam( i, LAMP_SIZE ) ]  = ParamForLampSize( kLampSizeDefault );
	}
	params[ PT_SCREEN ] = static_cast< float >( ScreenKind::AsIs );
	params[ PT_VIEW ]   = static_cast< float >( ViewKind::Picture );

	//---------------------------------------------------------------------
	// Declaration. Every ranged parameter is a plain 0..1 float, with the
	// conversions in Controls.cpp.
	//---------------------------------------------------------------------
	//FIRST, deliberately: Resolume Arena does not expose a mixer's parameter
	//0 (measured on genlock, wipe, relay, lenticular and pepper), so it holds
	//a control whose default is right if it can never be changed.
	SetParamInfo( PT_LINEAR_LIGHT, "Linear Light", FF_TYPE_BOOLEAN, true );
	SetParamInfof( PT_ROOM_LIGHT, "Room Light", FF_TYPE_STANDARD );

	//Named Opacity, and the name is load-bearing: Resolume binds a mixer
	//parameter called Opacity to the LAYER's opacity fader, and a layer
	//transition ramps it (measured on genlock, wipe and relay in Arena
	//7.27.1). So the occluder layer's fader brings the shadows in.
	SetParamInfof( PT_OPACITY, "Opacity", FF_TYPE_STANDARD );
	SetOptionParamInfo( PT_OCCLUDER, "Occluder", static_cast< unsigned >( OccluderKind::Count ), params[ PT_OCCLUDER ] );
	SetParamElementInfo( PT_OCCLUDER, 0, "Alpha", 0.0f );
	SetParamElementInfo( PT_OCCLUDER, 1, "Luma", 1.0f );
	SetParamInfof( PT_DISTANCE, "Distance", FF_TYPE_STANDARD );

	//Each lamp's colour is a triplet the host draws as one colour picker:
	//RED named for the lamp, then _Green and _Blue (the fleet's spelling).
	static const char* const lampNames[ kLamps ][ LAMP_FIELDS ] = {
		{ "Lamp 1", "Lamp 1_Green", "Lamp 1_Blue", "Lamp 1 X", "Lamp 1 Y", "Lamp 1 Size" },
		{ "Lamp 2", "Lamp 2_Green", "Lamp 2_Blue", "Lamp 2 X", "Lamp 2 Y", "Lamp 2 Size" },
		{ "Lamp 3", "Lamp 3_Green", "Lamp 3_Blue", "Lamp 3 X", "Lamp 3 Y", "Lamp 3 Size" },
	};
	static const char* const lampGroups[ kLamps ] = { "Lamp 1", "Lamp 2", "Lamp 3" };
	for( int i = 0; i < kLamps; ++i )
	{
		SetParamInfof( LampParam( i, LAMP_RED ), lampNames[ i ][ LAMP_RED ], FF_TYPE_RED );
		SetParamInfof( LampParam( i, LAMP_GREEN ), lampNames[ i ][ LAMP_GREEN ], FF_TYPE_GREEN );
		SetParamInfof( LampParam( i, LAMP_BLUE ), lampNames[ i ][ LAMP_BLUE ], FF_TYPE_BLUE );
		SetParamInfof( LampParam( i, LAMP_X ), lampNames[ i ][ LAMP_X ], FF_TYPE_STANDARD );
		SetParamInfof( LampParam( i, LAMP_Y ), lampNames[ i ][ LAMP_Y ], FF_TYPE_STANDARD );
		SetParamInfof( LampParam( i, LAMP_SIZE ), lampNames[ i ][ LAMP_SIZE ], FF_TYPE_STANDARD );
	}

	SetOptionParamInfo( PT_SCREEN, "Screen", static_cast< unsigned >( ScreenKind::Count ), params[ PT_SCREEN ] );
	SetParamElementInfo( PT_SCREEN, 0, "As Is", 0.0f );
	SetParamElementInfo( PT_SCREEN, 1, "On White", 1.0f );
	SetOptionParamInfo( PT_VIEW, "View", static_cast< unsigned >( ViewKind::Count ), params[ PT_VIEW ] );
	SetParamElementInfo( PT_VIEW, 0, "Picture", 0.0f );
	SetParamElementInfo( PT_VIEW, 1, "Light", 1.0f );

	// Groups, the way Resolume shows them: each group one contiguous run.
	for( FFUInt32 i = PT_LINEAR_LIGHT; i <= PT_ROOM_LIGHT; ++i )
		SetParamGroup( i, "Light" );
	for( FFUInt32 i = PT_OPACITY; i <= PT_DISTANCE; ++i )
		SetParamGroup( i, "Occluder" );
	for( int lamp = 0; lamp < kLamps; ++lamp )
		for( FFUInt32 f = 0; f < LAMP_FIELDS; ++f )
			SetParamGroup( LampParam( lamp, static_cast< LampField >( f ) ), lampGroups[ lamp ] );
	for( FFUInt32 i = PT_SCREEN; i <= PT_VIEW; ++i )
		SetParamGroup( i, "Screen" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Penumbra mixer" );

	diag::init();
	instance = ++gInstances;
}

//---------------------------------------------------------------------------
FFResult Penumbra::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	const std::string screen = fragmentOverride ? std::string( fragmentOverride ) : AssembleFragment( kScreenFragment );
	if( !compile( convertShader, AssembleFragment( kConvertFragment ), "convert" )
	    || !compile( prefixShader, AssembleFragment( kPrefixFragment ), "prefix" ) || !compile( screenShader, screen, "screen" ) )
	{
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised, viewport " + std::to_string( vp ? vp->width : 0 ) + "x" + std::to_string( vp ? vp->height : 0 )
	            + " (instance #" + std::to_string( instance ) + ")" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

void Penumbra::LogGuard( unsigned bit, const char* what )
{
	if( guardsLogged & bit )
		return;
	guardsLogged |= bit;
	diag::warn( std::string( "guard: called with " ) + what + " -- returned FF_FAIL (logged once)" );
}

Asked Penumbra::asked() const
{
	Asked a;
	a.linear   = params[ PT_LINEAR_LIGHT ] > 0.5f;
	a.room     = RoomFromParam( params[ PT_ROOM_LIGHT ] );
	a.opacity  = std::clamp( static_cast< double >( params[ PT_OPACITY ] ), 0.0, 1.0 );
	a.k        = DistanceFromParam( params[ PT_DISTANCE ] );
	a.occluder = static_cast< OccluderKind >( OptionIndex( params[ PT_OCCLUDER ], static_cast< int >( OccluderKind::Count ) ) );
	a.screen   = static_cast< ScreenKind >( OptionIndex( params[ PT_SCREEN ], static_cast< int >( ScreenKind::Count ) ) );
	a.view     = static_cast< ViewKind >( OptionIndex( params[ PT_VIEW ], static_cast< int >( ViewKind::Count ) ) );
	for( int i = 0; i < kLamps; ++i )
	{
		a.lamp[ i ].colour[ 0 ] = params[ LampParam( i, LAMP_RED ) ];
		a.lamp[ i ].colour[ 1 ] = params[ LampParam( i, LAMP_GREEN ) ];
		a.lamp[ i ].colour[ 2 ] = params[ LampParam( i, LAMP_BLUE ) ];
		a.lamp[ i ].x           = LampXFromParam( params[ LampParam( i, LAMP_X ) ] );
		a.lamp[ i ].y           = LampYFromParam( params[ LampParam( i, LAMP_Y ) ] );
		a.lamp[ i ].size        = LampSizeFromParam( params[ LampParam( i, LAMP_SIZE ) ] );
	}
	return a;
}

//---------------------------------------------------------------------------
FFResult Penumbra::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	//The SDK's Add example guards on the input count and on each pointer,
	//with a comment saying a host calls a mixer with one input while the
	//operator is still patching. Not observed in Arena (genlock, one
	//sequence) -- but a mixer that dereferenced a null would take Resolume
	//down with it. oxbow, an effect host, hands it one input: refused here.
	if( pGL == nullptr || pGL->inputTextures == nullptr )
	{
		LogGuard( 1u, "no input array" );
		return FF_FAIL;
	}
	if( pGL->numInputTextures < 2 )
	{
		LogGuard( 2u, ( std::to_string( pGL->numInputTextures ) + " input(s)" ).c_str() );
		return FF_FAIL;
	}
	if( pGL->inputTextures[ 0 ] == nullptr || pGL->inputTextures[ 1 ] == nullptr )
	{
		LogGuard( 4u, "a null input" );
		return FF_FAIL;
	}

	const FFGLTextureStruct& dest = *pGL->inputTextures[ 0 ];//the layer below: the screen
	const FFGLTextureStruct& src  = *pGL->inputTextures[ 1 ];//this layer: the occluder
	//HardwareWidth and HardwareHeight are the DENOMINATORS in
	//GetMaxGLTexCoords, so a zero there is an infinite MaxUV.
	if( dest.Width == 0 || dest.Height == 0 || dest.HardwareWidth == 0 || dest.HardwareHeight == 0 || src.Width == 0 || src.Height == 0
	    || src.HardwareWidth == 0 || src.HardwareHeight == 0 )
	{
		LogGuard( 8u, "a zero-sized input" );
		return FF_FAIL;
	}

	const int outW = static_cast< int >( currentViewport.width );
	const int outH = static_cast< int >( currentViewport.height );
	if( outW <= 0 || outH <= 0 )
	{
		LogGuard( 16u, "a zero viewport" );
		return FF_FAIL;
	}
	if( !screenShader.IsReady() || !convertShader.IsReady() || !prefixShader.IsReady() )
		return FF_FAIL;

	++frames;
	if( frames == 1 )
		diag::info( "first frame: Dest " + sizeText( dest ) + ", Src " + sizeText( src ) + ", out " + std::to_string( outW ) + "x"
		            + std::to_string( outH ) );
	if( params[ PT_OPACITY ] != loggedOpacity && opacityLines < kOpacityLines )
	{
		//The only evidence a host gives of what drives a mixer's Opacity:
		//the layer fader, a transition, or the mixer's own slider.
		diag::info( "Opacity " + std::to_string( params[ PT_OPACITY ] ) + " at frame " + std::to_string( frames ) + " (instance #"
		            + std::to_string( instance ) + ")" );
		loggedOpacity = params[ PT_OPACITY ];
		++opacityLines;
	}

	//-----------------------------------------------------------------
	// The room, as numbers. In double here; the shader gets floats.
	// The table covers Src's USED area -- its hardware padding is not
	// the occluder (kFaultPaddedTable builds the bug in).
	//-----------------------------------------------------------------
	const bool padded = ( fault & kFaultPaddedTable ) != 0;
	const int tableW  = static_cast< int >( padded ? src.HardwareWidth : src.Width );
	const int tableH  = static_cast< int >( padded ? src.HardwareHeight : src.Height );
	const bool shared = ( fault & kFaultSharedScale ) != 0;
	const Setup s     = Plan( asked(), outW, outH, tableW, tableH, shared ? static_cast< int >( dest.Width ) : tableW,
                          shared ? static_cast< int >( dest.Height ) : tableH, fault );
	lastSetup         = s;

	//Everything below changes GL state; all of it goes back on the way out,
	//whichever way out it is.
	ScopedGLState state;
	glDisable( GL_BLEND );

	//The buffers, before anything is bound for the frame.
	const bool keep = ( fault & kFaultKeepTable ) != 0 && table[ 0 ].Ready() && table[ 1 ].Ready();
	if( !keep && ( !table[ 0 ].Ensure( tableW + 1, tableH + 1 ) || !table[ 1 ].Ensure( tableW + 1, tableH + 1 ) ) )
	{
		LogGuard( 32u, ( "a table of " + std::to_string( tableW + 1 ) + "x" + std::to_string( tableH + 1 ) + " the GL would not make" ).c_str() );
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		return FF_FAIL;
	}
	if( tableW != loggedTableW || tableH != loggedTableH )
	{
		diag::info( "table " + std::to_string( tableW + 1 ) + "x" + std::to_string( tableH + 1 ) + ", Q " + std::to_string( s.levels ) + " (instance #"
		            + std::to_string( instance ) + ")" );
		loggedTableW = tableW;
		loggedTableH = tableH;
	}

	//-----------------------------------------------------------------
	// 1. Convert: the occluder as integers, into table 0.
	//-----------------------------------------------------------------
	glUseProgram( convertShader.GetGLID() );
	bindUnit( 0, src.Handle );
	convertShader.Set( "Occluder", 0 );
	convertShader.Set( "Luma", s.asked.occluder == OccluderKind::Luma ? 1 : 0 );
	convertShader.Set( "Levels", static_cast< int >( s.levels ) );
	drawInto( table[ 0 ], quad );

	//-----------------------------------------------------------------
	// 2. Prefix sums, along x then along y, ping-ponging the two tables.
	// After passes at strides 1, 16, 256, ... each texel holds the sum of
	// the 16^n texels at and before it; the loop stops once that covers
	// the axis.
	//-----------------------------------------------------------------
	glUseProgram( prefixShader.GetGLID() );
	prefixShader.Set( "Table", 0 );
	int from = 0;
	for( int axis = 0; axis < 2; ++axis )
	{
		const long n = axis == 0 ? tableW + 1 : tableH + 1;
		for( long stride = 1; stride < n; stride *= kRadix )
		{
			const int taps = static_cast< int >( std::min< long >( kRadix, ( n - 1 ) / stride + 1 ) );
			bindUnit( 0, table[ from ].Texture() );
			glUniform2i( prefixShader.FindUniform( "Step" ), axis == 0 ? static_cast< int >( stride ) : 0, axis == 1 ? static_cast< int >( stride ) : 0 );
			prefixShader.Set( "Taps", taps );
			drawInto( table[ 1 - from ], quad );
			from = 1 - from;
		}
	}

	//-----------------------------------------------------------------
	// 3. The screen, into the host's framebuffer at the host's viewport.
	//-----------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
	glViewport( state.saved.viewport[ 0 ], state.saved.viewport[ 1 ], state.saved.viewport[ 2 ], state.saved.viewport[ 3 ] );
	glUseProgram( screenShader.GetGLID() );
	bindUnit( 0, dest.Handle );
	bindUnit( 1, table[ from ].Texture() );
	screenShader.Set( "Screen", 0 );
	screenShader.Set( "Table", 1 );

	const FFGLTexCoords maxDest = GetMaxGLTexCoords( dest );
	screenShader.Set( "MaxUVScreen", maxDest.s, maxDest.t );
	screenShader.Set( "HalfTexelScreen", 0.5f / static_cast< float >( dest.Width ), 0.5f / static_cast< float >( dest.Height ) );
	screenShader.Set( "ScreenExact", static_cast< int >( dest.Width ) == outW && static_cast< int >( dest.Height ) == outH ? 1 : 0 );
	glUniform2i( screenShader.FindUniform( "OutSize" ), outW, outH );
	glUniform2i( screenShader.FindUniform( "TableSize" ), tableW, tableH );
	screenShader.Set( "Scale", static_cast< float >( s.scale[ 0 ] ), static_cast< float >( s.scale[ 1 ] ) );
	static const char* const offsetNames[ kLamps ] = { "Offset[0]", "Offset[1]", "Offset[2]" };
	static const char* const halfNames[ kLamps ]   = { "HalfSize[0]", "HalfSize[1]", "HalfSize[2]" };
	static const char* const shareNames[ kLamps ]  = { "Share[0]", "Share[1]", "Share[2]" };
	for( int i = 0; i < kLamps; ++i )
	{
		const Setup::Lamp& l = s.lamp[ i ];
		screenShader.Set( offsetNames[ i ], static_cast< float >( l.offset[ 0 ] ), static_cast< float >( l.offset[ 1 ] ) );
		screenShader.Set( halfNames[ i ], static_cast< float >( l.half[ 0 ] ), static_cast< float >( l.half[ 1 ] ) );
		screenShader.Set( shareNames[ i ], static_cast< float >( l.share[ 0 ] ), static_cast< float >( l.share[ 1 ] ),
		                  static_cast< float >( l.share[ 2 ] ) );
	}
	screenShader.Set( "Unblocked", static_cast< float >( s.unblocked[ 0 ] ), static_cast< float >( s.unblocked[ 1 ] ),
	                  static_cast< float >( s.unblocked[ 2 ] ) );
	screenShader.Set( "Opacity", static_cast< float >( s.asked.opacity ) );
	screenShader.Set( "Levels", static_cast< int >( s.levels ) );
	screenShader.Set( "Linear", s.asked.linear ? 1 : 0 );
	screenShader.Set( "OnWhite", s.asked.screen == ScreenKind::OnWhite ? 1 : 0 );
	screenShader.Set( "ShowLight", s.asked.view == ViewKind::Light ? 1 : 0 );
	screenShader.Set( "Fault", fault );

	quad.Draw();

	//Leave nothing bound: after DeInitGL a table left on unit 1 is a deleted
	//texture in the host's context.
	if( fault & kFaultLeaveBound )
		glActiveTexture( GL_TEXTURE0 );
	else
		unbindTextureUnits( 2 );
	glUseProgram( 0 );
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Penumbra::DeInitGL()
{
	if( frames > 0 )
		diag::info( "DeInitGL after " + std::to_string( frames ) + " frames, last Opacity " + std::to_string( params[ PT_OPACITY ] )
		            + " (instance #" + std::to_string( instance ) + ")" );
	convertShader.FreeGLResources();
	prefixShader.FreeGLResources();
	screenShader.FreeGLResources();
	quad.Release();
	table[ 0 ].Destroy();
	table[ 1 ].Destroy();
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Penumbra::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Penumbra::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Penumbra::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Penumbra::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

//---------------------------------------------------------------------------
void Penumbra::SetHostInfo( const char* hostname, const char* version )
{
	CFFGLPlugin::SetHostInfo( hostname, version );
	diag::info( std::string( "host=" ) + ( hostname ? hostname : "?" ) + " version=" + ( version ? version : "?" ) + " loaded from "
	            + diag::modulePath() );
}
