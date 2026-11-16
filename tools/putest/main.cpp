/**
    putest -- render SW Penumbra offline, and measure the shadows.

    The mixer gets TWO inputs: `inputTextures[0]` is Dest, the layer below --
    the screen, the clip printed on it (`--input-a`); `[1]` is Src, this layer
    -- the occluder (`--input-b`). They may be different sizes, with
    different hardware padding, rendered to a third.

        putest --out /tmp/f.png      a picture, through the real plugin
        putest --list                every parameter, for the sweep
        putest --names               no name over 16 bytes or duplicated; index 0 is Linear Light
        putest --mixer               the guards; two inputs, two sizes; the padding never occludes
        putest --unchanged           nothing blocked gives Dest, to the bit
        putest --colour              each shadow is exactly the sum of the lamps it can see
        putest --penumbra            the edge's ramp is s d / (D - d) wide; umbra; corners; the 3D cast
        putest --parting             each lamp's shadow sits at the occluder minus L g
        putest --alpha               the output alpha is the screen's; Luma reads a mask as Alpha does
        putest --linear              lamps add in linear light; the picker is sRGB
        putest --opacity             Opacity scales the blocking, in linear light
        putest --resize              a new Src or output size renders as a fresh instance does
        putest --mutation            one character of the shipped GLSL fails --penumbra
        putest --bench               ms/frame at 720p, 1080p and 4K
        putest --pipe                raw frames in, raw frames out (two inputs)

    PUTEST_RENDERER=software renders on Apple's software renderer instead of
    the GPU.

    Every check renders through the REAL plugin class in a headless CGL
    context and measures the property out of the picture against the
    harness's own model (Harness.h: a 3D ray cast through the occluder's
    plane), at 640x360 and at 320x180, and carries negative controls that
    build the wrong answer into the real plugin (`SetFaultForTest`) and show
    the check rejects it. Every tolerance is derived from something stated
    -- float ULPs of a coordinate, a counted number of roundings, GLSL 4.10's
    pow budget -- and AGENTS.md lists them.
*/

#include "Harness.h"

namespace
{

struct Raster
{
	int w, h;
};
constexpr Raster kRasters[ 2 ] = { { 640, 360 }, { 320, 180 } };

/// Run `check( W, H, fault )` at both rasters, clean, then with each fault
/// that must fail it.
template< typename F >
void atBothRasters( F&& check, std::initializer_list< std::pair< int, const char* > > faults )
{
	for( const Raster& r : kRasters )
	{
		std::printf( "\n %dx%d\n", r.w, r.h );
		check( r.w, r.h, kFaultNone );
		for( const auto& f : faults )
		{
			const int caught = failuresOf( [ & ] { check( r.w, r.h, f.first ); } );
			Negative( caught > 0, std::string( f.second ) + fmt( " fails %.0f assertion(s)", caught ) );
		}
	}
}

//---------------------------------------------------------------------------
// --list, --names
//---------------------------------------------------------------------------
int runList()
{
	Penumbra plugin;
	std::printf( "%-4s %-22s %-9s %10s   %-16s %s\n", "id", "name", "kind", "value", "range", "elements" );
	for( unsigned int id = 0; id < Penumbra::PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( id >= Penumbra::PT_ABOUT_FIRST )
		{
			std::printf( "%-4u %-22s %-9s %10s   %-16s %s\n", id, name ? name : "", "about", "-", "-", "the Stoatworks About block; not swept" );
			continue;
		}
		const char* kind = "standard";
		switch( plugin.GetParamType( id ) )
		{
		case FF_TYPE_BOOLEAN: kind = "boolean"; break;
		case FF_TYPE_EVENT: kind = "event"; break;
		case FF_TYPE_INTEGER: kind = "integer"; break;
		case FF_TYPE_OPTION: kind = "option"; break;
		case FF_TYPE_RED: kind = "red"; break;
		case FF_TYPE_GREEN: kind = "green"; break;
		case FF_TYPE_BLUE: kind = "blue"; break;
		case FF_TYPE_TEXT: kind = "text"; break;
		default: break;
		}
		RangeStruct range = plugin.GetParamRange( id );
		std::string elements;
		//An option's range reads back 0..1 whatever its element count.
		if( plugin.GetParamType( id ) == FF_TYPE_OPTION )
		{
			const unsigned n = std::max( 1u, plugin.GetNumParamElements( id ) );
			range.min        = 0.0f;
			range.max        = static_cast< float >( n ) - 1.0f;
			for( unsigned e = 0; e < n; ++e )
				elements += std::string( e ? " | " : "" ) + plugin.GetParamElementName( id, e );
		}
		char rangeText[ 32 ] = {};
		std::snprintf( rangeText, sizeof( rangeText ), "[%g .. %g]", range.min, range.max );
		std::printf( "%-4u %-22s %-9s %10.4f   %-16s %s\n", id, name ? name : "", kind, plugin.GetFloatParameter( id ), rangeText, elements.c_str() );
	}
	return 0;
}

int runNames()
{
	std::printf( "names: sixteen bytes at most, none twice, and the mixer's two load-bearing ones\n\n" );
	Penumbra mixer;
	int over = 0;
	std::vector< std::string > seen;
	for( unsigned int id = 0; id < Penumbra::PT_COUNT; ++id )
	{
		const char* name = mixer.GetParamName( id );
		if( name == nullptr )
			continue;
		if( std::strlen( name ) > 16 || std::find( seen.begin(), seen.end(), std::string( name ) ) != seen.end() )
		{
			std::printf( "  %-3u  %-28s %zu bytes%s\n", id, name, std::strlen( name ), std::strlen( name ) > 16 ? "" : ", duplicate" );
			++over;
		}
		seen.push_back( name );
	}
	Check( over == 0, fmt( "%.0f parameter names over 16 bytes or duplicated, of %.0f", over, Penumbra::PT_COUNT ) );

	const std::string id = names::kMixerId, shown = names::kMixerName;
	Check( id == "PU01" && shown == "SW Penumbra" && shown.size() <= 16,
	       "the plugin is " + shown + fmt( " (%.0f bytes), id ", shown.size() ) + id );

	//Index 0 is sacrificial: Arena does not expose a mixer's first parameter
	//(genlock, wipe, relay, lenticular, pepper), so its default must be right for ever.
	const char* first = mixer.GetParamName( 0 );
	Check( first != nullptr && std::strcmp( first, "Linear Light" ) == 0 && mixer.GetParamType( 0 ) == FF_TYPE_BOOLEAN && mixer.GetFloatParameter( 0 ) == 1.0f,
	       std::string( "parameter 0, which Arena hides, is " ) + ( first ? first : "(none)" ) + " (boolean, default on)" );
	const char* opacity = mixer.GetParamName( Penumbra::PT_OPACITY );
	Check( opacity != nullptr && std::strcmp( opacity, "Opacity" ) == 0 && mixer.GetParamType( Penumbra::PT_OPACITY ) == FF_TYPE_STANDARD
	           && mixer.GetFloatParameter( Penumbra::PT_OPACITY ) == 1.0f,
	       "the occluder's opacity is a standard parameter named Opacity (Arena binds it to the layer fader), default 1" );

	//The lamps' colours are triplets a host draws as one picker.
	bool triplets = true;
	for( int i = 0; i < kLamps; ++i )
		triplets = triplets && mixer.GetParamType( Penumbra::LampParam( i, Penumbra::LAMP_RED ) ) == FF_TYPE_RED
		           && mixer.GetParamType( Penumbra::LampParam( i, Penumbra::LAMP_GREEN ) ) == FF_TYPE_GREEN
		           && mixer.GetParamType( Penumbra::LampParam( i, Penumbra::LAMP_BLUE ) ) == FF_TYPE_BLUE;
	Check( triplets, "each lamp's colour is a RED, GREEN, BLUE triplet" );
	const bool options = std::strcmp( mixer.GetParamElementName( Penumbra::PT_OCCLUDER, 1 ), "Luma" ) == 0
	                     && std::strcmp( mixer.GetParamElementName( Penumbra::PT_SCREEN, 1 ), "On White" ) == 0
	                     && std::strcmp( mixer.GetParamElementName( Penumbra::PT_VIEW, 1 ), "Light" ) == 0;
	Check( options, "the options are Occluder Alpha|Luma, Screen As Is|On White, View Picture|Light" );
	const char* about = mixer.GetParamName( Penumbra::PT_ABOUT_FIRST );
	Check( about != nullptr && std::strcmp( about, "About" ) == 0 && Penumbra::PT_COUNT - Penumbra::PT_ABOUT_FIRST == stoatworks::about::kParamCount,
	       "the About block is last" );
	//The mappings invert to their defaults.
	Check( std::fabs( DistanceFromParam( ParamForDistance( kDistanceDefault ) ) - kDistanceDefault ) < 1e-6
	           && std::fabs( LampSizeFromParam( ParamForLampSize( kLampSizeDefault ) ) - kLampSizeDefault ) < 1e-6
	           && std::fabs( LampXFromParam( ParamForLampX( -0.45 ) ) + 0.45 ) < 1e-6 && OptionIndex( 1.0f, 2 ) == 1 && OptionIndex( 7.0f, 2 ) == 1,
	       "Distance, Lamp Size and Lamp X invert to their defaults; options map by index, clamped" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --mixer: the guards; two inputs of two sizes with two paddings; the
// screen's padding never shows and the occluder's never occludes; a block
// of Src throws its hard shadow on exactly the pixels it maps to; and the
// host's GL state comes back.
//---------------------------------------------------------------------------
void mixerCheck( int outW, int outH, int fault )
{
	const InputSpec dest = InputSpec::Padded( 200, 120, 256, 256 );
	const InputSpec src  = InputSpec::Padded( 96, 70, 128, 128 );
	const QuadCard card  = quadCard( dest.usedW, dest.usedH );

	MixRig rig;
	if( !rig.Init( outW, outH, dest, src, false, fault ) )
	{
		Check( false, "the rig initialises" );
		return;
	}
	if( !quiet )
		std::printf( "  out %dx%d; Dest %dx%d of %dx%d; Src %dx%d of %dx%d (both padded with opaque magenta)\n", outW, outH, dest.usedW, dest.usedH,
		             dest.hwW, dest.hwH, src.usedW, src.usedH, src.hwW, src.hwH );
	rig.UploadDest( card.image );

	//Nothing in Src: the screen as it is, sampled at its own MaxUV.
	rig.UploadSrc( flatField( src.usedW, src.usedH, kBlack, 0 ) );
	rig.Render();
	const Image clear = rig.Pixels();
	int sentinel = 0, checked = 0, worst = 0;
	for( int y = 0; y < outH; ++y )
		for( int x = 0; x < outW; ++x )
		{
			const Rgb seen = pixelAt( clear, outW, x, y );
			if( l1( seen, kSentinel ) < kSentinelNear )
				++sentinel;
			const double ux = ( x + 0.5 ) / outW * dest.usedW, vy = ( y + 0.5 ) / outH * dest.usedH;
			//Two Dest texels clear of the quadrant edges: bilinear's reach is one.
			if( std::fabs( ux - dest.usedW / 2 ) < 2.0 || std::fabs( vy - dest.usedH / 2 ) < 2.0 )
				continue;
			const Rgb want = card.quadrant[ ( vy >= dest.usedH / 2 ? 2 : 0 ) + ( ux >= dest.usedW / 2 ? 1 : 0 ) ];
			worst          = std::max( { worst, std::abs( seen.r - want.r ), std::abs( seen.g - want.g ), std::abs( seen.b - want.b ) } );
			++checked;
		}
	Check( sentinel == 0, fmt( "no padding of either input reached the picture (%.0f sentinel pixels)", sentinel ) );
	Check( checked > outW * outH / 2 && worst <= 1,
	       fmt( "each quadrant of the screen is its own flat colour, unshadowed (%.0f pixels, worst %.0f codes, tolerance 1)", checked, worst ) );

	//A block of Src, hard (Distance 0): black on exactly the pixels whose
	//centres map into it at SRC's scale, the clear render everywhere else.
	const Block block   = { 30, 20, 60, 45, 255 };
	rig.UploadSrc( blocksImage( src.usedW, src.usedH, { block } ) );
	rig.SetIndex( Penumbra::PT_DISTANCE, 0.0f );
	rig.Render();
	const Image hard = rig.Pixels();
	int inside = 0, wrongIn = 0, wrongOut = 0;
	double nearest = 1.0;
	for( int y = 0; y < outH; ++y )
		for( int x = 0; x < outW; ++x )
		{
			const double sx = ( x + 0.5 ) * src.usedW / outW, sy = ( y + 0.5 ) * src.usedH / outH;
			nearest         = std::min( { nearest, std::fabs( sx - std::round( sx ) ), std::fabs( sy - std::round( sy ) ) } );
			const bool in   = sx >= block.x0 && sx < block.x1 && sy >= block.y0 && sy < block.y1;
			const size_t at = at4( outW, x, y );
			if( in )
			{
				++inside;
				wrongIn += hard[ at ] != 0 || hard[ at + 1 ] != 0 || hard[ at + 2 ] != 0 || hard[ at + 3 ] != 255;
			}
			else
				wrongOut += std::memcmp( &hard[ at ], &clear[ at ], 4 ) != 0;
		}
	Check( nearest > 1e-3, fmt( "no pixel centre maps within 1e-3 texel of a Src texel edge (nearest %.4f)", nearest ) );
	Check( inside > 100 && wrongIn == 0, fmt( "the block's hard shadow is black on all %.0f pixels it maps to (%.0f wrong)", inside, wrongIn ) );
	Check( wrongOut == 0, fmt( "and every other pixel is the unshadowed screen, to the byte (%.0f differ)", wrongOut ) );
}

/// The host's GL state as the plugin found it: no texture on any unit it
/// used, unit 0 active, no program, the host's framebuffer, viewport, blend
/// and clear colour.
void stateCheck( int fault )
{
	MixRig rig;
	if( !rig.Init( 320, 180, false, fault ) )
		return;
	rig.UploadDest( screenCard( 320, 180 ) );
	rig.UploadSrc( figureCard( 320, 180 ) );
	rig.out.Begin();
	glEnable( GL_BLEND );
	glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO );
	glClearColor( 0.1f, 0.2f, 0.3f, 0.4f );
	glViewport( 0, 0, 320, 180 );
	const FFResult result = rig.plugin.ProcessOpenGL( &rig.process );
	GLint active = 0, bound[ 3 ] = { -1, -1, -1 }, program = -1, fbo = -1, viewport[ 4 ] = {}, src = 0, dst = 0;
	GLfloat clearColour[ 4 ] = {};
	glGetIntegerv( GL_ACTIVE_TEXTURE, &active );
	glGetIntegerv( GL_CURRENT_PROGRAM, &program );
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fbo );
	glGetIntegerv( GL_VIEWPORT, viewport );
	glGetIntegerv( GL_BLEND_SRC_RGB, &src );
	glGetIntegerv( GL_BLEND_DST_RGB, &dst );
	glGetFloatv( GL_COLOR_CLEAR_VALUE, clearColour );
	const bool blend = glIsEnabled( GL_BLEND );
	for( int u = 0; u < 3; ++u )
	{
		glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + u ) );
		glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound[ u ] );
	}
	glActiveTexture( GL_TEXTURE0 );
	glDisable( GL_BLEND );
	Check( result == FF_SUCCESS, "a frame renders" );
	Check( active == GL_TEXTURE0 && bound[ 0 ] == 0 && bound[ 1 ] == 0 && bound[ 2 ] == 0,
	       fmt( "after a frame, texture units 0, 1 and 2 are empty (%.0f, %.0f, %.0f)", bound[ 0 ], bound[ 1 ], bound[ 2 ] )
	           + fmt( " and unit 0 is active (unit %.0f)", active - GL_TEXTURE0 ) );
	Check( program == 0 && fbo == static_cast< GLint >( rig.out.fbo ) && viewport[ 2 ] == 320 && viewport[ 3 ] == 180,
	       "no program is left in use; the host's framebuffer and viewport are back" );
	Check( blend && src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA && clearColour[ 2 ] == 0.3f, "the host's blend and clear colour are back" );
}

int runMixer()
{
	std::printf( "a mixer takes two inputs, of two sizes, with two paddings\n\n" );
	{
		MixRig rig;
		if( !rig.Init( 320, 180 ) )
			return 1;
		rig.UploadDest( screenCard( 320, 180 ) );
		rig.UploadSrc( figureCard( 320, 180 ) );
		Check( rig.RenderBroken( 2, -2 ) == FF_FAIL, "a null input ARRAY returns FF_FAIL" );
		Check( rig.RenderBroken( 0, -1 ) == FF_FAIL, "zero input textures returns FF_FAIL" );
		Check( rig.RenderBroken( 1, -1 ) == FF_FAIL, "one input texture (an effect host's call) returns FF_FAIL" );
		Check( rig.RenderBroken( 2, 0 ) == FF_FAIL, "a null Dest returns FF_FAIL" );
		Check( rig.RenderBroken( 2, 1 ) == FF_FAIL, "a null Src returns FF_FAIL" );
		Check( rig.RenderBroken( 2, -1, true ) == FF_FAIL, "a zero-sized Dest returns FF_FAIL" );
		Check( rig.RenderBroken( 2, -1, false, true ) == FF_FAIL, "a zero viewport returns FF_FAIL" );
		Check( rig.Render(), "and two real inputs still render afterwards" );
	}
	std::printf( "\n" );
	stateCheck( kFaultNone );
	const int leaked = failuresOf( [ & ] { stateCheck( kFaultLeaveBound ); } );
	Negative( leaked > 0, fmt( "the table left bound on unit 1 fails %.0f assertion(s)", leaked ) );
	atBothRasters( mixerCheck, { { kFaultFoldedMaxUV, "Dest's MaxUV applied twice (the genlock trap)" },
	                             { kFaultSharedScale, "the occluder mapped at Dest's size, not Src's" },
	                             { kFaultPaddedTable, "the table built over Src's hardware size (the padding occludes)" },
	                             { kFaultHalfTexel, "the pixel's corner, not its centre, mapped into the occluder" } } );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --unchanged: wherever nothing is blocked the output IS Dest -- float for
// float, byte for byte -- whatever the lamps, the room and the alpha.
//---------------------------------------------------------------------------
void unchangedCheck( int W, int H, int fault )
{
	const ImageF screenF = randomPremultiplied( W, H, 11 );
	struct Case
	{
		const char* what;
		std::function< void( MixRig& ) > setup;
		Image src;
	};
	const double odd[ kLamps ][ 3 ] = { { 0.9, 0.6, 0.1 }, { 0.2, 0.7, 0.5 }, { 0.4, 0.1, 0.8 } };
	const std::vector< Block > block = { { W / 3, H / 3, W / 2, 2 * H / 3, 255 } };
	std::vector< Case > cases = {
		{ "Src transparent, default lamps", []( MixRig& ) {}, flatField( W, H, { 40, 80, 120 }, 0 ) },
		{ "an opaque occluder at Opacity 0", []( MixRig& r ) { r.SetIndex( Penumbra::PT_OPACITY, 0.0f ); }, flatField( W, H, kWhite, 255 ) },
		{ "every shadow thrown off the frame (lamps at the edges of their range, Distance 0.9)",
		  []( MixRig& r ) {
			  r.SetIndex( Penumbra::PT_DISTANCE, 1.0f );
			  const float xs[ 3 ] = { 0.0f, 1.0f, 1.0f }, ys[ 3 ] = { 0.0f, 1.0f, 0.0f };
			  for( int i = 0; i < kLamps; ++i )
			  {
				  r.SetIndex( Penumbra::LampParam( i, Penumbra::LAMP_X ), xs[ i ] );
				  r.SetIndex( Penumbra::LampParam( i, Penumbra::LAMP_Y ), ys[ i ] );
			  }
		  },
		  blocksImage( W, H, block ) },
		{ "odd lamp colours and Room Light 0.4, Src transparent",
		  [ & ]( MixRig& r ) {
			  r.SetIndex( Penumbra::PT_ROOM_LIGHT, ParamForRoom( 0.4 ) );
			  for( int i = 0; i < kLamps; ++i )
				  for( int c = 0; c < 3; ++c )
					  r.SetIndex( Penumbra::LampParam( i, static_cast< Penumbra::LampField >( c ) ), static_cast< float >( odd[ i ][ c ] ) );
		  },
		  flatField( W, H, kBlack, 0 ) },
		{ "Linear Light off, Src transparent", []( MixRig& r ) { r.SetIndex( Penumbra::PT_LINEAR_LIGHT, 0.0f ); }, flatField( W, H, kBlack, 0 ) },
	};

	for( const Case& c : cases )
	{
		//Float in, float out: partial alpha everywhere.
		MixRig rig;
		if( !rig.Init( W, H, InputSpec::Exact( W, H ), InputSpec::Exact( W, H ), true, fault, nullptr, true ) )
			return;
		c.setup( rig );
		rig.UploadDestF( screenF );
		rig.UploadSrc( c.src );
		rig.Render();
		const size_t differ = differingFloats( rig.PixelsF(), screenF );
		Check( differ == 0, std::string( c.what ) + fmt( ": float Dest with partial alpha comes back bit for bit (%.0f floats differ)", differ ) );
	}
	{
		//8-bit in, 8-bit out, and Dest padded at the output's size.
		MixRig rig;
		if( !rig.Init( W, H, InputSpec::Padded( W, H, W + 64, H + 40 ), InputSpec::Exact( W, H ), false, fault ) )
			return;
		const Image screen = screenCard( W, H, true );
		rig.UploadDest( screen );
		rig.UploadSrc( flatField( W, H, kBlack, 0 ) );
		rig.SetIndex( Penumbra::PT_ROOM_LIGHT, ParamForRoom( 0.4 ) );
		rig.SetIndex( Penumbra::LampParam( 1, Penumbra::LAMP_RED ), 0.5f );
		rig.Render();
		const size_t differ = differingBytes( rig.Pixels(), screen );
		Check( differ == 0, fmt( "8-bit Dest with alpha, padded at the output's size, comes back byte for byte (%.0f differ)", differ ) );
	}
	{
		//On White: the clip over white, the same two float operations.
		MixRig rig;
		if( !rig.Init( W, H, InputSpec::Exact( W, H ), InputSpec::Exact( W, H ), true, fault, nullptr, true ) )
			return;
		rig.SetIndex( Penumbra::PT_SCREEN, static_cast< float >( ScreenKind::OnWhite ) );
		rig.UploadDestF( screenF );
		rig.UploadSrc( flatField( W, H, kBlack, 0 ) );
		rig.Render();
		ImageF want = screenF;
		for( size_t i = 0; i < want.size(); i += 4 )
		{
			const float clear = 1.0f - screenF[ i + 3 ];
			for( int c = 0; c < 3; ++c )
				want[ i + static_cast< size_t >( c ) ] = screenF[ i + static_cast< size_t >( c ) ] + clear;
			want[ i + 3 ] = 1.0f;
		}
		const size_t differ = differingFloats( rig.PixelsF(), want );
		Check( differ == 0, fmt( "On White, nothing blocked: rgb + ( 1 - a ), alpha 1, bit for bit (%.0f floats differ)", differ ) );
	}
}

int runUnchanged()
{
	std::printf( "nothing blocked gives the screen unchanged, to the bit\n" );
	atBothRasters( unchangedCheck, { { kFaultReencode, "every channel decoded and re-encoded even at light 1" },
	                                 { kFaultUnbalanced, "the light not white-balanced to the unblocked screen" } } );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --colour: a block and three lamps placed so the shadows overlap in every
// combination. In each region's interior the light is
// ( R + the unblocked lamps ) / ( R + all of them ), per channel.
//---------------------------------------------------------------------------
Room colourRoom( int W, int H )
{
	Room r;
	r.W = W, r.H = H, r.mw = W, r.mh = H;
	r.k = 0.2;
	triangle( r, 0.05, kPrimaries );
	const int side = static_cast< int >( 0.4 * H );
	r.blocks       = { { W / 2 - side / 2, H / 2 - side / 2, W / 2 + side / 2, H / 2 + side / 2, 255 } };
	return r;
}

/// Which lamps a pixel is wholly blocked from (bit i), or -1 if any lamp is
/// partly blocked or within 1e-3 texel of a block edge.
int regionOf( const Room& r, int x, int y )
{
	int code = 0;
	for( int i = 0; i < kLamps; ++i )
	{
		if( r.edgeMargin( i, x + 0.5, y + 0.5 ) < 1e-3 )
			return -1;
		const double b = r.blocked( i, x + 0.5, y + 0.5 );
		if( b == 1.0 )
			code |= 1 << i;
		else if( b != 0.0 )
			return -1;
	}
	return code;
}

void colourCheck( int W, int H, int fault )
{
	//Red, green and blue on a white screen: every region's colour exactly.
	for( int linear = 1; linear >= 0; --linear )
	{
		Room r = colourRoom( W, H );
		MixRig rig;
		if( !rig.Init( W, H, false, fault ) )
			return;
		r.linear = linear != 0;
		r.Apply( rig );
		rig.UploadDest( flatField( W, H, kWhite ) );
		rig.UploadSrc( blocksImage( W, H, r.blocks ) );
		rig.Render();
		const Image out = rig.Pixels();
		int count[ 8 ] = {}, wrong = 0;
		for( int y = 0; y < H; ++y )
			for( int x = 0; x < W; ++x )
			{
				const int code = regionOf( r, x, y );
				if( code < 0 )
					continue;
				++count[ code ];
				const size_t at = at4( W, x, y );
				for( int c = 0; c < 3; ++c )
					wrong += out[ at + static_cast< size_t >( c ) ] != ( ( code >> c ) & 1 ? 0 : 255 );
			}
		int regions = 0;
		for( int code = 0; code < 8; ++code )
			regions += count[ code ] >= 20;
		Check( regions == 8, std::string( linear ? "Linear Light on" : "Linear Light off" )
		                         + fmt( ": all eight regions present (none, R, G, B, RG, RB, GB, RGB blocked), %.0f with 20+ pixels", regions ) );
		Check( wrong == 0, std::string( linear ? "Linear Light on" : "Linear Light off" )
		                       + fmt( ": block red and the screen is exactly cyan, green magenta, blue yellow, two lamps their third, all three black (%.0f channel values wrong of ", wrong )
		                       + fmt( "%.0f pixels)", count[ 1 ] + count[ 2 ] + count[ 3 ] + count[ 4 ] + count[ 5 ] + count[ 6 ] + count[ 7 ] + count[ 0 ] ) );
	}

	//Other lamps, read as light: ( R + unblocked ) / ( R + all ).
	struct Set
	{
		const char* what;
		double colours[ kLamps ][ 3 ];
		double room;
	};
	const Set sets[] = {
		{ "amber, teal, violet", { { 1.0, 0.75, 0.0 }, { 0.0, 0.5, 0.5 }, { 0.5, 0.0, 1.0 } }, 0.0 },
		{ "two whites and a black lamp", { { 1.0, 1.0, 1.0 }, { 1.0, 1.0, 1.0 }, { 0.0, 0.0, 0.0 } }, 0.0 },
		{ "unequal levels, Room Light 0.3", { { 0.8, 0.2, 0.4 }, { 0.3, 0.9, 0.1 }, { 0.2, 0.4, 0.7 } }, 0.3 },
	};
	for( const Set& set : sets )
	{
		Room r = colourRoom( W, H );
		triangle( r, 0.05, set.colours );
		r.room = set.room;
		MixRig rig;
		if( !rig.Init( W, H, true, fault ) )
			return;
		r.Apply( rig );
		showLight( rig, true );
		rig.SetIndex( Penumbra::PT_LINEAR_LIGHT, 0.0f );
		rig.UploadDest( flatField( W, H, kWhite ) );
		rig.UploadSrc( blocksImage( W, H, r.blocks ) );
		rig.Render();
		const ImageF out = rig.PixelsF();
		double worst     = 0.0;
		int checked      = 0;
		for( int y = 0; y < H; ++y )
			for( int x = 0; x < W; ++x )
			{
				if( regionOf( r, x, y ) < 0 )
					continue;
				++checked;
				for( int c = 0; c < 3; ++c )
					worst = std::max( worst, std::fabs( channel( out, W, x, y, c ) - r.light( c, x + 0.5, y + 0.5 ) ) );
			}
		Check( checked > W * H / 2 && worst <= kColourTol,
		       std::string( set.what ) + fmt( ": every region is ( R + the unblocked lamps ) / ( R + all ) (%.0f pixels, worst %.2g, tolerance %.2g)", checked, worst, kColourTol ) );
	}
}

int runColour()
{
	std::printf( "each shadow's colour is exactly the sum of the lamps it can see\n" );
	atBothRasters( colourCheck, { { kFaultProduct, "the shadows multiplied, not summed" },
	                              { kFaultUnbalanced, "the light not white-balanced" },
	                              { kFaultTowardLamp, "the shadows thrown towards their lamps" } } );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --penumbra: the edge of a shadow is a straight ramp s d / ( D - d ) wide;
// a narrow bar has no umbra; a corner is the product of its two edges; and
// every pixel agrees with the 3D ray cast.
//---------------------------------------------------------------------------
struct Fit
{
	int n        = 0;
	double slope = 0.0, at = 0.0;///< L = at + slope x
	double mean       = 0.0;     ///< the x the points are centred on
	double slopeBound = 0.0;     ///< the slope's worst error from a per-point error of 1: sum |x - mean| / sum ( x - mean )^2
};

Fit fitLine( const std::vector< double >& xs, const std::vector< double >& ls )
{
	Fit f;
	f.n = static_cast< int >( xs.size() );
	if( f.n < 2 )
		return f;
	double mx = 0, ml = 0;
	for( int i = 0; i < f.n; ++i )
		mx += xs[ i ], ml += ls[ i ];
	mx /= f.n, ml /= f.n;
	double sxx = 0, sxl = 0, sabs = 0;
	for( int i = 0; i < f.n; ++i )
	{
		sxx += ( xs[ i ] - mx ) * ( xs[ i ] - mx );
		sxl += ( xs[ i ] - mx ) * ( ls[ i ] - ml );
		sabs += std::fabs( xs[ i ] - mx );
	}
	f.slope      = sxl / sxx;
	f.at         = ml - f.slope * mx;
	f.slopeBound = sabs / sxx;
	f.mean       = mx;
	return f;
}

/// One lamp, white, at ( 0.3, 0.1 ); the other two black.
Room oneLamp( int W, int H, double k, double size )
{
	Room r;
	r.W = W, r.H = H, r.mw = W, r.mh = H;
	r.k            = k;
	r.lamp[ 0 ]    = { { 1.0, 1.0, 1.0 }, 0.3, 0.1, size };
	r.lamp[ 1 ]    = { { 0.0, 0.0, 0.0 }, 0.0, 0.3, 0.05 };
	r.lamp[ 2 ]    = { { 0.0, 0.0, 0.0 }, 0.45, -0.15, 0.05 };
	return r;
}

ImageF renderLight( MixRig& rig, Room& r )
{
	r.Apply( rig );
	showLight( rig, true );
	rig.SetIndex( Penumbra::PT_LINEAR_LIGHT, r.linear ? 1.0f : 0.0f );
	rig.UploadDest( flatField( r.W, r.H, kWhite ) );
	rig.UploadSrc( blocksImage( r.mw, r.mh, r.blocks ) );
	rig.Render();
	return rig.PixelsF();
}

void penumbraCheck( int W, int H, int fault, const char* fragment = nullptr )
{
	MixRig rig;
	if( !rig.Init( W, H, true, fault, fragment ) )
	{
		Check( false, "the rig initialises" );
		return;
	}

	//An edge: the left 40% of the frame occluded.
	struct Case
	{
		double k, size;
	};
	for( const Case c : { Case{ 0.2, 0.2 }, Case{ 0.4, 0.2 }, Case{ 0.6, 0.1 }, Case{ 0.2, 0.4 } } )
	{
		Room r   = oneLamp( W, H, c.k, c.size );
		r.blocks = { { 0, 0, static_cast< int >( 0.4 * W ), H, 255 } };
		const ImageF out = renderLight( rig, r );
		//Similar triangles: the lamp's two sides throw the edge s d / ( D - d )
		//apart on the screen, D any height and d = k D. In pixels, times H.
		const double d    = r.k * r.D;
		const double want = r.lamp[ 0 ].size * d / ( r.D - d ) * H;
		std::vector< double > xs, ls;
		const int y = H / 2;
		for( int x = 0; x < W; ++x )
		{
			const double l = channel( out, W, x, y, 0 );
			if( l > 1e-3 && l < 1.0 - 1e-3 )
				xs.push_back( x + 0.5 ), ls.push_back( l );
		}
		const Fit f        = fitLine( xs, ls );
		const double width = f.slope != 0.0 ? 1.0 / std::fabs( f.slope ) : 0.0;
		const double level = r.levelBound( 0 );
		const double tol   = 3.0 * width * width * f.slopeBound * level;
		Check( f.n >= 4 && std::fabs( width - want ) <= tol,
		       fmt( "k %.1f, lamp %.2f: the edge is a ramp %.4f px wide, s d / (D - d) is %.4f px", r.k, r.lamp[ 0 ].size, width, want )
		           + fmt( " (%.0f pixels on it, tolerance %.2g)", f.n, tol ) );
		//A point off by e from a true line, against a fit whose value at the
		//mean is off by e and whose slope is off by slopeBound e.
		double residual = 0.0, residualTol = 0.0;
		for( size_t i = 0; i < xs.size(); ++i )
		{
			residual    = std::max( residual, std::fabs( ls[ i ] - ( f.at + f.slope * xs[ i ] ) ) );
			residualTol = std::max( residualTol, 3.0 * level * ( 2.0 + std::fabs( xs[ i ] - f.mean ) * f.slopeBound ) );
		}
		Check( residual <= residualTol, fmt( "  and straight: worst residual %.2g, tolerance %.2g", residual, residualTol ) );
	}

	//A bar narrower than the penumbra: no umbra, and its darkest is 1 - a / w.
	{
		Room r       = oneLamp( W, H, 0.5, 0.3 );//w = 0.3 H
		const int a  = std::max( 4, static_cast< int >( 0.06 * H ) );
		const int x0 = W / 2 - a / 2;
		r.blocks     = { { x0, 0, x0 + a, H, 255 } };
		const ImageF out = renderLight( rig, r );
		const double d   = r.k * r.D;
		const double w   = r.lamp[ 0 ].size * d / ( r.D - d ) * H;
		const double want = 1.0 - a / w;
		const double tol  = 3.0 * r.levelBound( 0 );
		int plateau       = 0;
		double worst = 0.0, lowest = 1.0;
		const int y = H / 2;
		for( int x = 0; x < W; ++x )
		{
			const double l = channel( out, W, x, y, 0 );
			lowest         = std::min( lowest, l );
			double bx0, by0, bx1, by1;
			r.boxAt( 0, x + 0.5, y + 0.5, bx0, by0, bx1, by1 );
			if( bx0 <= x0 - 1e-3 && bx1 >= x0 + a + 1e-3 )
				++plateau, worst = std::max( worst, std::fabs( l - want ) );
		}
		Check( plateau >= 3 && worst <= tol && lowest >= want - tol,
		       fmt( "a bar %.0f px wide under a %.2f px penumbra has no umbra: darkest %.6f, 1 - a / w is %.6f", a, w, lowest, want )
		           + fmt( " (%.0f plateau pixels, worst %.2g)", plateau, worst ) );
	}

	//A corner: the product of its two edges' fractions.
	{
		Room r = oneLamp( W, H, 0.4, 0.2 );
		const int cx = W / 2, cy = H / 2;
		r.blocks     = { { 0, 0, cx, cy, 255 } };
		const ImageF out = renderLight( rig, r );
		const double tol = 3.0 * r.levelBound( 0 );
		int corner       = 0;
		double worst     = 0.0;
		for( int y = 0; y < H; ++y )
			for( int x = 0; x < W; ++x )
			{
				double bx0, by0, bx1, by1;
				r.boxAt( 0, x + 0.5, y + 0.5, bx0, by0, bx1, by1 );
				const double fx = std::clamp( ( cx - bx0 ) / ( bx1 - bx0 ), 0.0, 1.0 );
				const double fy = std::clamp( ( cy - by0 ) / ( by1 - by0 ), 0.0, 1.0 );
				if( fx <= 0.0 || fx >= 1.0 || fy <= 0.0 || fy >= 1.0 )
					continue;
				++corner;
				worst = std::max( worst, std::fabs( channel( out, W, x, y, 0 ) - ( 1.0 - fx * fy ) ) );
			}
		Check( corner > 50 && worst <= tol, fmt( "a corner's shadow is the product of its edges' ramps (%.0f pixels, worst %.2g, tolerance %.2g)", corner, worst, tol ) );
	}

	//Everything at once, pixel by pixel against the ray cast: three lamps of
	//three colours and sizes, blocks at three alphas, three of them on the
	//frame's edges and corners where boxes hang off it (cut on one side or
	//on two), Opacity 0.8, Room Light 0.2, and
	//Src at its own size AND its own aspect (3/4 of the width, 1/2 of the
	//height), so a square lamp is a rectangle in occluder texels.
	{
		Room r;
		r.W = W, r.H = H, r.mw = W * 3 / 4, r.mh = H / 2;
		r.k            = 0.35;
		r.room         = 0.2;
		r.opacity      = 0.8;
		r.lamp[ 0 ]    = { { 1.0, 0.6, 0.2 }, -0.5, -0.2, 0.12 };
		r.lamp[ 1 ]    = { { 0.1, 0.9, 0.4 }, 0.1, 0.35, 0.25 };
		r.lamp[ 2 ]    = { { 0.3, 0.2, 1.0 }, 0.6, -0.1, 0.06 };
		r.blocks       = { { r.mw / 5, r.mh / 4, r.mw / 2, r.mh * 3 / 4, 255 },
                     { r.mw / 2 + 6, r.mh / 3, r.mw * 3 / 4, r.mh / 2, 153 },
                     { r.mw * 3 / 5, r.mh * 3 / 5, r.mw * 4 / 5, r.mh * 4 / 5, 51 },
                     { 0, r.mh / 8, r.mw / 8, r.mh / 3, 255 },
                     { 0, 0, r.mw / 10, r.mh / 10, 255 },
                     { r.mw * 7 / 8, r.mh * 5 / 6, r.mw, r.mh, 255 } };
		MixRig cast;
		if( !cast.Init( W, H, InputSpec::Exact( W, H ), InputSpec::Exact( r.mw, r.mh ), true, fault, fragment ) )
			return;
		const ImageF out = renderLight( cast, r );
		double tol       = 0.0;
		for( int i = 0; i < kLamps; ++i )
			tol += 3.0 * r.levelBound( i );
		double worst = 0.0;
		int wx = 0, wy = 0, wc = 0;
		for( int y = 0; y < H; ++y )
			for( int x = 0; x < W; ++x )
				for( int c = 0; c < 3; ++c )
				{
					const double e = std::fabs( channel( out, W, x, y, c ) - r.light( c, x + 0.5, y + 0.5 ) );
					if( e > worst )
						worst = e, wx = x, wy = y, wc = c;
				}
		Check( worst <= tol, fmt( "every pixel and channel matches the 3D ray cast (D %.1f, k %.2f; worst %.2g, tolerance %.2g)", r.D, r.k, worst, tol )
		                         + fmt( ", at pixel %.0f, %.0f channel %.0f", wx, wy, wc )
		                         + fmt( ": %.6f against %.6f", channel( out, W, wx, wy, wc ), r.light( wc, wx + 0.5, wy + 0.5 ) ) );
	}
}

int runPenumbra()
{
	std::printf( "penumbra width matches the similar-triangles closed form\n" );
	atBothRasters( []( int W, int H, int fault ) { penumbraCheck( W, H, fault ); },
	               { { kFaultOccluderPlane, "the box s k wide (the lamp scaled to the occluder's plane)" },
	                 { kFaultPointLamps, "every lamp a point" } } );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --parting: each lamp's shadow is the occluder moved by -L g. Its edges,
// fitted from the picture at half light, sit where the ray cast puts them.
//---------------------------------------------------------------------------
void partingCheck( int W, int H, int fault )
{
	for( const double k : { 0.0, 0.2, 0.5 } )
	{
		Room r;
		r.W = W, r.H = H, r.mw = W, r.mh = H;
		r.k = k;
		triangle( r, 0.15, kPrimaries );
		const int side = static_cast< int >( 0.2 * H );
		const Block b  = { W / 2 - side / 2, H / 2 - side / 2, W / 2 + side / 2, H / 2 + side / 2, 255 };
		r.blocks       = { b };
		MixRig rig;
		if( !rig.Init( W, H, true, fault ) )
			return;
		const ImageF out = renderLight( rig, r );
		const double g   = r.k / ( 1.0 - r.k );

		if( k == 0.0 )
		{
			//At the screen every lamp's shadow is the occluder itself.
			int wrong = 0;
			for( int y = 0; y < H; ++y )
				for( int x = 0; x < W; ++x )
				{
					const bool in = x >= b.x0 && x < b.x1 && y >= b.y0 && y < b.y1;
					for( int c = 0; c < 3; ++c )
						wrong += channel( out, W, x, y, c ) != ( in ? 0.0 : 1.0 );
				}
			Check( wrong == 0, fmt( "k 0: all three shadows are exactly the occluder, hard (%.0f channel values off)", wrong ) );
			continue;
		}

		double worst = 0.0, tolWorst = 0.0;
		bool fitted  = true;
		for( int i = 0; i < kLamps; ++i )
		{
			//Where the shadow's centre is predicted: the block moved by -L g.
			const double cx = 0.5 * ( b.x0 + b.x1 ) - r.lamp[ i ].x * g * H;
			const double cy = 0.5 * ( b.y0 + b.y1 ) - r.lamp[ i ].y * g * H;
			const double level = r.levelBound( i );
			for( int axis = 0; axis < 2; ++axis )
			{
				double edges[ 2 ];
				for( int side2 = 0; side2 < 2; ++side2 )
				{
					std::vector< double > xs, ls;
					const int n = axis == 0 ? W : H;
					for( int t = 0; t < n; ++t )
					{
						const double centre = axis == 0 ? cx : cy;
						if( ( side2 == 0 ) != ( t + 0.5 < centre ) )
							continue;
						const int x    = axis == 0 ? t : static_cast< int >( std::floor( cx ) );
						const int y    = axis == 0 ? static_cast< int >( std::floor( cy ) ) : t;
						const double l = channel( out, W, x, y, i );
						if( l > 1e-3 && l < 1.0 - 1e-3 )
							xs.push_back( t + 0.5 ), ls.push_back( l );
					}
					const Fit f = fitLine( xs, ls );
					fitted      = fitted && f.n >= 3;
					edges[ side2 ] = f.slope != 0.0 ? ( 0.5 - f.at ) / f.slope : 0.0;
					//The crossing is mean + ( 0.5 - L(mean) ) / slope. A level
					//error e per point moves L(mean) by e, which moves the
					//crossing e w; and moves the slope by slopeBound e, which
					//moves it | crossing - mean | slopeBound e w.
					const double width = f.slope != 0.0 ? 1.0 / std::fabs( f.slope ) : 0.0;
					const double reach = std::fabs( edges[ side2 ] - f.mean );
					tolWorst           = std::max( tolWorst, 3.0 * level * width * ( 1.0 + reach * f.slopeBound ) );
				}
				const double got  = 0.5 * ( edges[ 0 ] + edges[ 1 ] );
				const double want = axis == 0 ? cx : cy;
				worst             = std::max( worst, std::fabs( got - want ) );
			}
		}
		Check( fitted && worst <= tolWorst,
		       fmt( "k %.1f: each lamp's shadow is centred on the occluder moved by -L g, in x and y (worst %.2g px, tolerance %.2g)", k, worst, tolWorst ) );
	}
}

int runParting()
{
	std::printf( "the shadows part by (L_i - L_j) g as the occluder steps towards the lamps\n" );
	atBothRasters( partingCheck, { { kFaultTowardLamp, "the shadow thrown towards its lamp" },
	                               { kFaultHalfTexel, "the pixel's corner, not its centre, mapped into the occluder" } } );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --alpha: the output's alpha is the screen's; a shadow on a partly
// transparent pixel is lit straight and premultiplied back; On White is
// opaque; Luma reads a grey mask exactly as Alpha reads the same values.
//---------------------------------------------------------------------------
void alphaCheck( int W, int H, int fault )
{
	const ImageF screenF = randomPremultiplied( W, H, 29 );
	Room r;
	r.W = W, r.H = H, r.mw = W, r.mh = H;
	r.k    = 0.2;
	r.room = 0.5;
	r.linear = true;
	triangle( r, 0.0, kPrimaries );//point lamps: every pixel wholly lit or wholly blocked
	const int side = static_cast< int >( 0.35 * H );
	r.blocks       = { { W / 2 - side / 2, H / 2 - side / 2, W / 2 + side / 2, H / 2 + side / 2, 255 } };

	ImageF opaque, half;
	for( int pass = 0; pass < 2; ++pass )
	{
		MixRig rig;
		if( !rig.Init( W, H, InputSpec::Exact( W, H ), InputSpec::Exact( W, H ), true, fault, nullptr, true ) )
			return;
		r.Apply( rig );
		rig.UploadDestF( screenF );
		std::vector< Block > blocks = r.blocks;
		blocks[ 0 ].a               = pass == 0 ? 255 : 128;
		rig.UploadSrc( blocksImage( W, H, blocks ) );
		rig.Render();
		( pass == 0 ? opaque : half ) = rig.PixelsF();
	}
	size_t alphaDiffers = 0, rgbDiffers = 0;
	for( size_t i = 3; i < screenF.size(); i += 4 )
	{
		alphaDiffers += std::memcmp( &opaque[ i ], &screenF[ i ], sizeof( float ) ) != 0;
		alphaDiffers += std::memcmp( &half[ i ], &screenF[ i ], sizeof( float ) ) != 0;
		rgbDiffers += opaque[ i - 3 ] != half[ i - 3 ];
	}
	Check( alphaDiffers == 0, fmt( "As Is: the output alpha is Dest's, bit for bit, shadowed or not (%.0f differ)", alphaDiffers ) );
	Check( rgbDiffers > 100, fmt( "and the occluder's alpha (0.5 against 1) changes the colour (%.0f pixels), never the alpha", rgbDiffers ) );

	//A shadowed pixel with partial alpha: encode( decode( rgb / a ) L ) a.
	double worst = 0.0;
	int checked  = 0;
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			const int code = regionOf( r, x, y );
			const size_t at = at4( W, x, y );
			const double a  = screenF[ at + 3 ];
			if( code <= 0 || a < 0.05 )
				continue;
			++checked;
			for( int c = 0; c < 3; ++c )
			{
				const double L    = r.light( c, x + 0.5, y + 0.5 );
				const double want = L == 1.0 ? screenF[ at + static_cast< size_t >( c ) ]
				                             : linearToSrgb( srgbToLinear( std::clamp( screenF[ at + static_cast< size_t >( c ) ] / a, 0.0, 1.0 ) ) * L ) * a;
				worst = std::max( worst, std::fabs( opaque[ at + static_cast< size_t >( c ) ] - want ) );
			}
		}
	Check( checked > 100 && worst <= kPowTol,
	       fmt( "a shadow on partial alpha is encode( decode( rgb / a ) L ) a (%.0f pixels, worst %.2g, tolerance %.2g)", checked, worst, kPowTol ) );

	//On White: opaque everywhere.
	{
		MixRig rig;
		if( !rig.Init( W, H, InputSpec::Exact( W, H ), InputSpec::Exact( W, H ), true, fault, nullptr, true ) )
			return;
		r.Apply( rig );
		rig.SetIndex( Penumbra::PT_SCREEN, static_cast< float >( ScreenKind::OnWhite ) );
		rig.UploadDestF( screenF );
		rig.UploadSrc( blocksImage( W, H, r.blocks ) );
		rig.Render();
		const ImageF out = rig.PixelsF();
		size_t notOne    = 0;
		for( size_t i = 3; i < out.size(); i += 4 )
			notOne += out[ i ] != 1.0f;
		Check( notOne == 0, fmt( "On White: the output alpha is 1 everywhere (%.0f pixels not)", notOne ) );
	}

	//Luma: a grey mask with no alpha is the same occluder as those greys in
	//alpha -- the same bytes out, soft lamps and three alpha levels.
	{
		Image byAlpha, byLuma;
		const std::vector< Block > blocks = { { W / 4, H / 4, W / 2, H * 3 / 4, 255 }, { W / 2 + 4, H / 3, W * 3 / 4, H * 2 / 3, 153 },
			                                  { W / 3, H / 8, W * 2 / 3, H / 5, 51 } };
		for( int pass = 0; pass < 2; ++pass )
		{
			MixRig rig;
			if( !rig.Init( W, H, false, fault ) )
				return;
			rig.SetIndex( Penumbra::PT_OCCLUDER, static_cast< float >( pass == 0 ? OccluderKind::Alpha : OccluderKind::Luma ) );
			rig.UploadDest( screenCard( W, H, true ) );
			rig.UploadSrc( blocksImage( W, H, blocks, pass == 1 ) );
			rig.Render();
			( pass == 0 ? byAlpha : byLuma ) = rig.Pixels();
		}
		const size_t differ = differingBytes( byAlpha, byLuma );
		const size_t shaded = differingBytes( byAlpha, screenCard( W, H, true ) );
		Check( differ == 0 && shaded > 100,
		       fmt( "Occluder Luma on a grey-on-black opaque mask renders the bytes Alpha renders from the same greys in alpha (%.0f differ; %.0f shaded)", differ, shaded ) );
	}
}

int runAlpha()
{
	std::printf( "the output alpha is the screen's, decided, on clips with alpha\n" );
	atBothRasters( alphaCheck, { { kFaultAlphaFromSrc, "the output alpha taken from the occluder" },
	                             { kFaultStraight, "Dest decoded without un-premultiplying" } } );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --linear: three white lamps, one blocked, over a grey ramp that crosses
// sRGB's knee; and a picked colour's share in linear light.
//---------------------------------------------------------------------------
void linearCheck( int W, int H, int fault )
{
	//Lamp 1 a point at the reference point, its shadow the block itself;
	//lamps 2 and 3 at the ends of their range with Distance 0.9, their
	//shadows far off the frame. Inside the block exactly one of three
	//equal lamps is blocked: L = 2/3.
	auto room = [ & ]( const double colours[ kLamps ][ 3 ] ) {
		Room r;
		r.W = W, r.H = H, r.mw = W, r.mh = H;
		r.k         = kDistanceMax;
		r.lamp[ 0 ] = { { colours[ 0 ][ 0 ], colours[ 0 ][ 1 ], colours[ 0 ][ 2 ] }, 0.0, 0.0, 0.0 };
		r.lamp[ 1 ] = { { colours[ 1 ][ 0 ], colours[ 1 ][ 1 ], colours[ 1 ][ 2 ] }, -kLampXMax, 0.0, 0.05 };
		r.lamp[ 2 ] = { { colours[ 2 ][ 0 ], colours[ 2 ][ 1 ], colours[ 2 ][ 2 ] }, kLampXMax, 0.0, 0.05 };
		r.blocks    = { { W / 10, H / 10, W * 9 / 10, H * 9 / 10, 255 } };
		return r;
	};
	ImageF ramp( static_cast< size_t >( W ) * H * 4 );
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			const float g = 0.12f * static_cast< float >( x ) / static_cast< float >( W );
			for( int c = 0; c < 3; ++c )
				ramp[ at4( W, x, y ) + static_cast< size_t >( c ) ] = g;
			ramp[ at4( W, x, y ) + 3 ] = 1.0f;
		}

	const double whites[ kLamps ][ 3 ] = { { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 } };
	for( int linear = 1; linear >= 0; --linear )
	{
		Room r   = room( whites );
		r.linear = linear != 0;
		MixRig rig;
		if( !rig.Init( W, H, InputSpec::Exact( W, H ), InputSpec::Exact( W, H ), true, fault, nullptr, true ) )
			return;
		r.Apply( rig );
		rig.SetIndex( Penumbra::PT_LINEAR_LIGHT, r.linear ? 1.0f : 0.0f );
		rig.UploadDestF( ramp );
		rig.UploadSrc( blocksImage( W, H, r.blocks ) );
		rig.Render();
		const ImageF out = rig.PixelsF();
		double worst     = 0.0;
		int checked = 0, belowKnee = 0;
		for( int y = 0; y < H; ++y )
			for( int x = 0; x < W; ++x )
			{
				if( regionOf( r, x, y ) != 1 )
					continue;
				const double g    = ramp[ at4( W, x, y ) ];
				const double L    = r.light( 0, x + 0.5, y + 0.5 );
				const double want = r.linear ? linearToSrgb( srgbToLinear( g ) * L ) : g * L;
				worst             = std::max( worst, std::fabs( channel( out, W, x, y, 1 ) - want ) );
				++checked;
				belowKnee += g <= 0.04045;
			}
		const double tol = r.linear ? kPowTol : kColourTol;
		Check( checked > W * H / 2 && belowKnee > 100 && worst <= tol,
		       std::string( r.linear ? "Linear Light on: one of three white lamps blocked is encode( decode( g ) 2/3 )"
		                             : "Linear Light off: one of three white lamps blocked is g 2/3" )
		           + fmt( " across the knee (%.0f pixels, %.0f below 0.04045; worst %.2g, tolerance %.2g)", checked, belowKnee, worst, tol ) );
	}

	//A lamp picked at ( 1, 0.5, 0 ) beside a white one: its green share is
	//decode( 0.5 ) / ( decode( 0.5 ) + 1 ), 0.176, not 0.5 / 1.5.
	{
		const double picked[ kLamps ][ 3 ] = { { 1.0, 0.5, 0.0 }, { 1, 1, 1 }, { 0, 0, 0 } };
		Room r   = room( picked );
		r.linear = true;
		MixRig rig;
		if( !rig.Init( W, H, true, fault ) )
			return;
		const ImageF out = renderLight( rig, r );
		double worst     = 0.0;
		int checked      = 0;
		for( int y = 0; y < H; ++y )
			for( int x = 0; x < W; ++x )
			{
				if( regionOf( r, x, y ) != 1 )
					continue;
				++checked;
				for( int c = 0; c < 3; ++c )
					worst = std::max( worst, std::fabs( channel( out, W, x, y, c ) - linearToSrgb( r.light( c, x + 0.5, y + 0.5 ) ) ) );
			}
		Check( checked > W * H / 2 && worst <= kPowTol,
		       fmt( "a lamp picked at (1, 0.5, 0) shares decode(0.5) = %.4f of green with a white lamp: light %.4f where it is blocked", srgbToLinear( 0.5 ),
		            1.0 / ( 1.0 + srgbToLinear( 0.5 ) ) )
		           + fmt( " (worst %.2g, tolerance %.2g)", worst, kPowTol ) );
	}
}

int runLinear()
{
	std::printf( "the lamps add in linear light, and the picker is sRGB\n" );
	//Reencode is --unchanged's: it touches only channels at light 1, which
	//this check, measuring inside the shadow, does not read.
	atBothRasters( linearCheck, { { kFaultGammaShares, "lamp colours used as picked, not decoded" } } );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --opacity: the layer fader scales how much each lamp is blocked: 1 - L is
// linear in Opacity, and the picture is lit by that light in linear light,
// not crossfaded in gamma space.
//---------------------------------------------------------------------------
void opacityCheck( int W, int H, int fault )
{
	Room r;
	r.W = W, r.H = H, r.mw = W, r.mh = H;
	r.k = 0.3;
	triangle( r, 0.2, kPrimaries );
	const int side = static_cast< int >( 0.3 * H );
	r.blocks       = { { W / 2 - side / 2, H / 2 - side / 2, W / 2 + side / 2, H / 2 + side / 2, 255 } };

	MixRig rig;
	if( !rig.Init( W, H, true, fault ) )
		return;
	r.opacity          = 1.0;
	const ImageF full  = renderLight( rig, r );
	double worstLinear = 0.0, worstPicture = 0.0;
	int checked        = 0;
	for( const double op : { 0.25, 0.5 } )
	{
		r.opacity        = op;
		const ImageF out = renderLight( rig, r );

		//The picture: a grey 0.5 screen, Linear Light on.
		MixRig pic;
		if( !pic.Init( W, H, InputSpec::Exact( W, H ), InputSpec::Exact( W, H ), true, fault, nullptr, true ) )
			return;
		Room rp   = r;
		rp.linear = true;
		rp.Apply( pic );
		ImageF grey( static_cast< size_t >( W ) * H * 4, 0.5f );
		for( size_t i = 3; i < grey.size(); i += 4 )
			grey[ i ] = 1.0f;
		pic.UploadDestF( grey );
		pic.UploadSrc( blocksImage( W, H, r.blocks ) );
		pic.Render();
		const ImageF picture = pic.PixelsF();

		for( int y = 0; y < H; ++y )
			for( int x = 0; x < W; ++x )
				for( int c = 0; c < 3; ++c )
				{
					const double blockedFull = 1.0 - channel( full, W, x, y, c );
					if( blockedFull < 0.01 || blockedFull > 0.99 )
						continue;
					++checked;
					worstLinear     = std::max( worstLinear, std::fabs( ( 1.0 - channel( out, W, x, y, c ) ) - op * blockedFull ) );
					const double L  = 1.0 - op * blockedFull;
					worstPicture    = std::max( worstPicture, std::fabs( channel( picture, W, x, y, c ) - linearToSrgb( srgbToLinear( 0.5 ) * L ) ) );
				}
	}
	Check( checked > 200 && worstLinear <= kColourTol,
	       fmt( "1 - light is Opacity times the blocking at 0.25 and 0.5, in the penumbrae (%.0f values, worst %.2g, tolerance %.2g)", checked, worstLinear, kColourTol ) );
	Check( worstPicture <= kPowTol + kColourTol,
	       fmt( "and a grey screen under it is encode( decode( 0.5 ) L ), not a gamma-space crossfade (worst %.2g, tolerance %.2g)", worstPicture, kPowTol + kColourTol ) );

	//Opacity 0: the screen as it is.
	{
		MixRig zero;
		if( !zero.Init( W, H, false, fault ) )
			return;
		zero.SetIndex( Penumbra::PT_OPACITY, 0.0f );
		const Image screen = screenCard( W, H );
		zero.UploadDest( screen );
		zero.UploadSrc( blocksImage( W, H, r.blocks ) );
		zero.Render();
		Check( differingBytes( zero.Pixels(), screen ) == 0, "Opacity 0 under an opaque occluder: the screen, byte for byte" );
	}
}

int runOpacity()
{
	std::printf( "Opacity is the occluder's: it scales the blocking, in linear light\n" );
	atBothRasters( opacityCheck, { { kFaultCrossfade, "Opacity as a gamma-space crossfade to the shadowed picture" } } );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --resize: the two tables are sized to Src. A Src or output size changed
// mid-run renders exactly what a fresh instance renders at the new sizes.
//---------------------------------------------------------------------------
void resizeCheck( int fault )
{
	struct Step
	{
		int outW, outH, srcW, srcH;
	};
	const Step steps[] = { { 640, 360, 320, 200 }, { 640, 360, 640, 360 }, { 640, 360, 800, 450 }, { 320, 180, 400, 240 } };
	MixRig rig;
	if( !rig.Init( steps[ 0 ].outW, steps[ 0 ].outH, InputSpec::Exact( steps[ 0 ].outW, steps[ 0 ].outH ), InputSpec::Exact( steps[ 0 ].srcW, steps[ 0 ].srcH ),
	               false, fault ) )
		return;
	bool first = true;
	for( const Step& s : steps )
	{
		if( !first )
		{
			if( s.outW != rig.out.width || s.outH != rig.out.height )
				rig.ResizeOutput( s.outW, s.outH );
			rig.MakeInputs( InputSpec::Exact( s.outW, s.outH ), InputSpec::Exact( s.srcW, s.srcH ) );
		}
		first = false;
		rig.UploadDest( screenCard( s.outW, s.outH ) );
		rig.UploadSrc( figureCard( s.srcW, s.srcH ) );
		rig.Render();
		const Image running = rig.Pixels();

		MixRig fresh;
		if( !fresh.Init( s.outW, s.outH, InputSpec::Exact( s.outW, s.outH ), InputSpec::Exact( s.srcW, s.srcH ), false, kFaultNone ) )
			return;
		fresh.UploadDest( screenCard( s.outW, s.outH ) );
		fresh.UploadSrc( figureCard( s.srcW, s.srcH ) );
		fresh.Render();
		const size_t differ = differingBytes( running, fresh.Pixels() );
		Check( differ == 0, fmt( "out %.0fx%.0f, Src %.0fx", s.outW, s.outH, s.srcW ) + fmt( "%.0f mid-run: a fresh instance's bytes (%.0f differ)", s.srcH, differ ) );
	}
}

int runResize()
{
	std::printf( "a size changed mid-run renders as a fresh instance does\n\n" );
	resizeCheck( kFaultNone );
	const int caught = failuresOf( [] { resizeCheck( kFaultKeepTable ); } );
	Negative( caught > 0, fmt( "the table never reallocated after the first frame fails %.0f assertion(s)", caught ) );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --mutation
//---------------------------------------------------------------------------
int runMutation()
{
	std::printf( "one character of the shipped screen shader fails a check\n\n" );
	const std::string shipped = AssembleFragment( kScreenFragment );
	const std::string from    = "vec3( float( i0 + 1 ) - a, 1.0, b - float( i1 ) )";
	const std::string to      = "vec3( float( i0 - 1 ) - a, 1.0, b - float( i1 ) )";
	const size_t at           = shipped.find( from );
	Check( at != std::string::npos && shipped.find( from, at + 1 ) == std::string::npos, "the line to mutate occurs exactly once in the shipped shader" );
	if( at == std::string::npos )
		return 1;
	std::string mutated = shipped;
	mutated.replace( at, from.size(), to );
	int differ = 0;
	for( size_t i = 0; i < shipped.size(); ++i )
		differ += shipped[ i ] != mutated[ i ];
	Check( differ == 1, fmt( "the mutation is one character (%.0f differ): a box's first part-texel weighted from i0 - 1, not i0 + 1", differ ) );

	Image defaultBytes, hookBytes;
	for( int pass = 0; pass < 2; ++pass )
	{
		MixRig rig;
		if( !rig.Init( 320, 180, false, kFaultNone, pass ? shipped.c_str() : nullptr ) )
			return 1;
		rig.UploadDest( screenCard( 320, 180 ) );
		rig.UploadSrc( figureCard( 320, 180 ) );
		rig.Render();
		( pass ? hookBytes : defaultBytes ) = rig.Pixels();
	}
	Check( differingBytes( defaultBytes, hookBytes ) == 0, "the shipped text through the hook renders the default path's bytes" );

	const int shippedFails = failuresOf( [ & ] { penumbraCheck( 320, 180, kFaultNone, shipped.c_str() ); } );
	Check( shippedFails == 0, "--penumbra passes with the shipped text through the hook" );
	const int mutatedFails = failuresOf( [ & ] { penumbraCheck( 320, 180, kFaultNone, mutated.c_str() ); } );
	Negative( mutatedFails > 0, fmt( "--penumbra fails the mutated shader (%.0f assertion(s))", mutatedFails ) );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( int width, int height, int frames, const std::vector< std::string >& extra )
{
	MixRig rig;
	if( !rig.Init( width, height ) )
		return -1.0;
	rig.UploadDest( screenCard( width, height ) );
	rig.UploadSrc( figureCard( width, height ) );
	for( const std::string& s : extra )
	{
		const size_t eq = s.find( '=' );
		rig.Set( s.substr( 0, eq ), std::strtof( s.substr( eq + 1 ).c_str(), nullptr ) );
	}
	for( int frame = 0; frame < 5; ++frame )
		rig.Render();
	glFinish();
	const auto start = std::chrono::steady_clock::now();
	for( int frame = 0; frame < frames; ++frame )
		rig.Render();
	glFinish();
	const auto end = std::chrono::steady_clock::now();
	return std::chrono::duration< double >( end - start ).count() * 1000.0 / frames;
}

int runBench( int frames )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = { { "1280x720 ", 1280, 720 }, { "1920x1080", 1920, 1080 }, { "3840x2160", 3840, 2160 } };
	std::printf( "%d frames each after a 5-frame warm-up, glFinish both sides; Dest and Src at the output size, the figure as the occluder.\n", frames );
	std::printf( "defaults (Distance 0.35, lamps 0.05), then Distance 0.9 with lamps 0.5 (the biggest boxes the controls allow).\n\n" );
	std::printf( "resolution     defaults ms/frame   %% of 60fps   | biggest boxes ms/frame   %% of 60fps\n" );
	const std::vector< std::string > biggest = { "Distance=1", "Lamp 1 Size=1", "Lamp 2 Size=1", "Lamp 3 Size=1" };
	for( const Size& size : sizes )
	{
		const double plain = benchAt( size.width, size.height, frames, {} );
		const double big   = benchAt( size.width, size.height, frames, biggest );
		if( plain < 0.0 || big < 0.0 )
			return 1;
		std::printf( "%s        %7.3f          %5.1f%%     |        %7.3f           %5.1f%%\n", size.name, plain, plain / 16.667 * 100.0, big, big / 16.667 * 100.0 );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --set, strictly: a value is a number, all of it, or an option's element
// name. Anything else is refused, never read as 0.
//---------------------------------------------------------------------------
bool applyOne( Penumbra& plugin, const std::string& setting )
{
	const size_t equals = setting.find( '=' );
	if( equals == std::string::npos )
	{
		std::fprintf( stderr, "--set %s: expected Name=Value\n", setting.c_str() );
		return false;
	}
	const std::string name = setting.substr( 0, equals ), text = setting.substr( equals + 1 );
	for( unsigned id = 0; id < Penumbra::PT_ABOUT_FIRST; ++id )
	{
		const char* declared = plugin.GetParamName( id );
		if( declared == nullptr || name != declared )
			continue;
		char* end         = nullptr;
		const float value = std::strtof( text.c_str(), &end );
		if( !text.empty() && end != nullptr && *end == '\0' && std::isfinite( value ) )
		{
			plugin.SetFloatParameter( id, value );
			return true;
		}
		if( plugin.GetParamType( id ) == FF_TYPE_OPTION )
			for( unsigned e = 0; e < plugin.GetNumParamElements( id ); ++e )
				if( text == plugin.GetParamElementName( id, e ) )
				{
					plugin.SetFloatParameter( id, static_cast< float >( e ) );
					return true;
				}
		std::fprintf( stderr, "--set %s: '%s' is not a number%s\n", setting.c_str(), text.c_str(),
		              plugin.GetParamType( id ) == FF_TYPE_OPTION ? " or one of the option's names (try --list)" : "" );
		return false;
	}
	std::fprintf( stderr, "--set %s: no parameter called '%s' (try --list)\n", setting.c_str(), name.c_str() );
	return false;
}

bool applySettings( Penumbra& plugin, const std::vector< std::string >& settings )
{
	for( const std::string& s : settings )
		if( !applyOne( plugin, s ) )
			return false;
	return true;
}

//---------------------------------------------------------------------------
// --pipe: the fleet's frame format, with a second input, in relay's,
// lenticular's and pepper's shape.
//
//   * stdin is DEST, inputTextures[0], the layer below: the screen.
//     Output-sized, raw RGBA, top row first.
//   * `--pipe-src PATH` is SRC, inputTextures[1], this layer: the occluder.
//     Raw RGBA, top row first, at `--src-size` (default: the output size).
//     A FIFO will do. Without it the `--input-b` card is held.
//
// `--script` is the cue sheet: `frame Name value`, value in host units, '#'
// a comment; a STANDARD (or colour) parameter ramps between keys, an option
// or a boolean steps. Unknown names are refused before a frame is read.
// SIGPIPE is ignored: a closed stdout is exit 1.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		char* end         = nullptr;
		const float value = std::strtof( words.back().c_str(), &end );
		if( end == nullptr || *end != '\0' )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": '" + words.back() + "' is not a number";
			return {};
		}
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame, bool ramps )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame < track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			const auto& b = track[ i ];
			if( !ramps )
				return a.second;
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
		if( frame == track[ i ].first )
			return track[ i ].second;
	}
	return track.back().second;
}

bool readFrame( int fd, Image& frame )
{
	size_t filled = 0;
	while( filled < frame.size() )
	{
		const ssize_t got = read( fd, frame.data() + filled, frame.size() - filled );
		if( got <= 0 )
			return false;
		filled += static_cast< size_t >( got );
	}
	return true;
}

bool writeAll( int fd, const Image& frame )
{
	size_t written = 0;
	while( written < frame.size() )
	{
		const ssize_t n = write( fd, frame.data() + written, frame.size() - written );
		if( n <= 0 )
			return false;
		written += static_cast< size_t >( n );
	}
	return true;
}

void flipRows( const Image& in, Image& out, int width, int height )
{
	const size_t row = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( out.data() + static_cast< size_t >( height - 1 - y ) * row, in.data() + static_cast< size_t >( y ) * row, row );
}

int runPipe( int width, int height, int srcWidth, int srcHeight, const std::string& scriptPath, const std::string& srcPath, const std::string& inputB,
             const std::vector< std::string >& settings )
{
	signal( SIGPIPE, SIG_IGN );
	MixRig rig;
	if( !rig.Init( width, height, InputSpec::Exact( width, height ), InputSpec::Exact( srcWidth, srcHeight ) ) )
		return 1;
	if( !applySettings( rig.plugin, settings ) )
		return 2;

	struct Automation
	{
		unsigned int id;
		bool ramps;
		Track track;
	};
	std::vector< Automation > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
		if( !error.empty() )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
		for( const auto& entry : tracks )
		{
			bool found = false;
			for( unsigned int id = 0; id < Penumbra::PT_ABOUT_FIRST; ++id )
			{
				const char* name = rig.plugin.GetParamName( id );
				if( name != nullptr && entry.first == name )
				{
					const unsigned int type = rig.plugin.GetParamType( id );
					const bool ramps        = type == FF_TYPE_STANDARD || type == FF_TYPE_RED || type == FF_TYPE_GREEN || type == FF_TYPE_BLUE;
					automation.push_back( { id, ramps, entry.second } );
					found = true;
					break;
				}
			}
			if( !found )
			{
				std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
				return 2;
			}
		}
	}

	int srcFd = -1;
	if( !srcPath.empty() )
	{
		srcFd = open( srcPath.c_str(), O_RDONLY );
		if( srcFd < 0 )
		{
			std::fprintf( stderr, "cannot open --pipe-src %s\n", srcPath.c_str() );
			return 2;
		}
	}
	else
		rig.UploadSrc( generate( inputB, srcWidth, srcHeight ) );

	Image destIn( static_cast< size_t >( width ) * height * 4 ), destUp( destIn.size() );
	Image srcIn( static_cast< size_t >( srcWidth ) * srcHeight * 4 ), srcUp( srcIn.size() );
	Image out( destIn.size() );
	int index  = 0;
	int result = 0;
	for( ;; ++index )
	{
		if( !readFrame( STDIN_FILENO, destIn ) )
			break;
		if( srcFd >= 0 )
		{
			if( !readFrame( srcFd, srcIn ) )
				break;
			flipRows( srcIn, srcUp, srcWidth, srcHeight );
			rig.UploadSrc( srcUp );
		}
		flipRows( destIn, destUp, width, height );
		rig.UploadDest( destUp );
		for( const Automation& track : automation )
			rig.plugin.SetFloatParameter( track.id, valueAt( track.track, index, track.ramps ) );
		if( !rig.Render() )
		{
			std::fprintf( stderr, "ProcessOpenGL failed at frame %d\n", index );
			result = 1;
			break;
		}
		flipRows( rig.Pixels(), out, width, height );
		if( !writeAll( STDOUT_FILENO, out ) )
		{
			std::fprintf( stderr, "putest --pipe: stdout closed at frame %d\n", index );
			result = 1;
			break;
		}
	}
	if( srcFd >= 0 )
		close( srcFd );
	std::fprintf( stderr, "putest --pipe: %d frames\n", index );
	return result;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"putest -- render and measure SW Penumbra (FFGL mixer)\n"
		"\n"
		"  --out PATH        render through the plugin (default penumbra.png)\n"
		"  --input-a NAME    DEST, the screen: the clip printed on it (default screen)\n"
		"  --input-b NAME    SRC, the occluder (default figure)\n"
		"                    %s\n"
		"  --src-size WxH    Src's size (default: the output size)\n"
		"  --size WxH        picture size (default 1280x720)\n"
		"  --frames N        frames to render for --out (stateless: the last is written), or --bench's count\n"
		"  --set \"Name=V\"    a parameter by display name, in host units, or an option by its\n"
		"                    element name (\"View=Light\"). Repeatable. Unparsable values are refused.\n"
		"  --list            every parameter, its kind, default and range\n"
		"  --names --mixer --unchanged --colour --penumbra --parting --alpha --linear\n"
		"  --opacity --resize --mutation\n"
		"                    the checks (see the top of tools/putest/main.cpp)\n"
		"  --bench           ms/frame at 720p, 1080p and 4K (--frames N, default 30)\n"
		"  --pipe            raw RGBA Dest frames on stdin, raw RGBA frames on stdout\n"
		"  --pipe-src PATH   raw RGBA Src frames for --pipe (a file or FIFO); default: --input-b, held\n"
		"  --script PATH     cues for --pipe: 'frame Name value'; standard and colour parameters ramp, the rest step\n"
		"  --help\n",
		kCardNames );
}

bool parseSize( const std::string& text, int& w, int& h )
{
	const size_t x = text.find( 'x' );
	if( x == std::string::npos )
		return false;
	char* end = nullptr;
	w         = static_cast< int >( std::strtol( text.substr( 0, x ).c_str(), &end, 10 ) );
	if( end == nullptr || *end != '\0' )
		return false;
	h = static_cast< int >( std::strtol( text.substr( x + 1 ).c_str(), &end, 10 ) );
	return end != nullptr && *end == '\0' && w > 0 && h > 0;
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "penumbra.png";
	std::string inputA = "screen", inputB = "figure";
	int width = 1280, height = 720, srcWidth = 0, srcHeight = 0, frames = 0;
	bool wantList = false, wantBench = false, wantPipe = false;
	std::string scriptPath, srcPath, check;
	std::vector< std::string > settings;
	static const char* checks[] = { "--names", "--mixer", "--unchanged", "--colour", "--penumbra", "--parting",
		                            "--alpha", "--linear", "--opacity", "--resize", "--mutation" };

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		bool isCheck               = false;
		for( const char* c : checks )
			isCheck = isCheck || argument == c;
		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( isCheck )
			check = argument;
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( ( argument == "--input-a" || argument == "--input-b" ) && hasNext )
		{
			const std::string name = argv[ ++i ];
			if( !isCard( name ) )
			{
				std::fprintf( stderr, "%s: no card called '%s' (%s)\n", argument.c_str(), name.c_str(), kCardNames );
				return 2;
			}
			( argument == "--input-a" ? inputA : inputB ) = name;
		}
		else if( ( argument == "--size" || argument == "--src-size" ) && hasNext )
		{
			if( !parseSize( argv[ ++i ], argument == "--size" ? width : srcWidth, argument == "--size" ? height : srcHeight ) )
			{
				std::fprintf( stderr, "%s wants a positive WxH\n", argument.c_str() );
				return 2;
			}
		}
		else if( argument == "--frames" && hasNext )
		{
			char* end        = nullptr;
			const long value = std::strtol( argv[ ++i ], &end, 10 );
			if( end == nullptr || *end != '\0' || value <= 0 )
			{
				std::fprintf( stderr, "--frames wants a positive whole number\n" );
				return 2;
			}
			frames = static_cast< int >( value );
		}
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--pipe-src" && hasNext )
			srcPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}
	if( srcWidth == 0 )
	{
		srcWidth  = width;
		srcHeight = height;
	}

	if( wantList )
		return runList();
	if( check == "--names" )
		return runNames();

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	int result = 0;
	if( check == "--mixer" )
		result = runMixer();
	else if( check == "--unchanged" )
		result = runUnchanged();
	else if( check == "--colour" )
		result = runColour();
	else if( check == "--penumbra" )
		result = runPenumbra();
	else if( check == "--parting" )
		result = runParting();
	else if( check == "--alpha" )
		result = runAlpha();
	else if( check == "--linear" )
		result = runLinear();
	else if( check == "--opacity" )
		result = runOpacity();
	else if( check == "--resize" )
		result = runResize();
	else if( check == "--mutation" )
		result = runMutation();
	else if( wantBench )
		result = runBench( frames > 0 ? frames : 30 );
	else if( wantPipe )
		result = runPipe( width, height, srcWidth, srcHeight, scriptPath, srcPath, inputB, settings );
	else
	{
		MixRig rig;
		if( !rig.Init( width, height, InputSpec::Exact( width, height ), InputSpec::Exact( srcWidth, srcHeight ) ) )
			return 1;
		rig.UploadDest( generate( inputA, width, height ) );
		rig.UploadSrc( generate( inputB, srcWidth, srcHeight ) );
		if( !applySettings( rig.plugin, settings ) )
			return 2;
		bool ok = true;
		for( int f = 0; f < std::max( 1, frames ); ++f )
			ok = rig.Render() && ok;
		if( !ok || !writePng( outPath, width, height, rig.Pixels() ) )
		{
			std::fprintf( stderr, "could not render or write %s\n", outPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s\n", outPath.c_str() );
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	if( result != 0 )
		std::printf( "\n%d check(s) FAILED\n", failures > 0 ? failures : result );
	return result;
}
