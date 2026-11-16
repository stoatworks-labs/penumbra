#pragma once

#include <FFGLSDK.h>

//AFTER the SDK: this header names FFUInt32 and does not pull the SDK in
//itself, so an include placed above it fails with "unknown type name" errors
//that point at the About block rather than at the include order.
#include "StoatworksAboutParams.h"
#include "Model.h"
#include "TableBuffer.h"

#include <string>

/**
    Penumbra -- coloured shadows: three coloured lamps light a white screen
    with the clip printed on it, and someone stands between them and the
    screen. An FFGL **mixer** for Resolume.

    Dest, the layer below, is the screen. Src, the layer whose blend mode
    this is, is the occluder: the hub's person mask, or any layer's alpha.
    Each screen point shows the sum of the lamps it can see, so red, green
    and blue lamps throw cyan, magenta and yellow shadows, black only where
    all three are blocked, parting and softening as the person steps
    towards the lamps. See Model.h and AGENTS.md.

    **This is the fleet's sixth FF_MIXER.** Read the mixer section of
    genlock's AGENTS.md before changing `ProcessOpenGL`: what is written
    there was measured, and several things the SDK's headers imply are not
    true.

    It has no state from one frame to the next. It does hold two buffers
    (the summed-area table's ping-pong), sized to Src, rebuilt in full every
    frame and reallocated when Src's size changes (`--resize`).
*/
class Penumbra : public CFFGLPlugin
{
public:
	Penumbra();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	/// Changes no pixel. It exists so that ONE session in front of Resolume
	/// leaves a log that says which host and which bundle.
	void SetHostInfo( const char* hostname, const char* version ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Optional, not required -- see genlock's AGENTS.md. A four-character
	/// label for a host with no room for "SW Penumbra".
	const char* GetShortName() override
	{
		return "Pnmb";
	}

	/// What the last rendered frame actually used, for the harness. A check
	/// that disagrees with the picture can then say WHICH side is wrong.
	const penumbra::Setup& SetupForTest() const
	{
		return lastSetup;
	}

	/// What the parameters ask for, in physical units, through Controls.
	penumbra::Asked AskedForTest() const
	{
		return asked();
	}

	/// The negative controls: a bitmask of penumbra::Fault, 0 in the shipped
	/// plugin, reachable only from here. Each one builds the wrong answer to
	/// a check into the real ProcessOpenGL or the real shader, so the check
	/// can be shown to reject the plugin's own wrong version.
	void SetFaultForTest( int mask )
	{
		fault = mask;
	}

	/// The mutation test: compile this screen-pass fragment shader instead of
	/// the shipped one. Call before InitGL. Null restores the shipped text.
	void SetFragmentShaderForTest( const char* text )
	{
		fragmentOverride = text;
	}

	/// The lamps' parameters, in the order each lamp's block declares them.
	enum LampField : FFUInt32
	{
		LAMP_RED,
		LAMP_GREEN,
		LAMP_BLUE,
		LAMP_X,
		LAMP_Y,
		LAMP_SIZE,
		LAMP_FIELDS
	};

	/// In the order the host shows them.
	///
	/// **Index 0 is sacrificial.** Resolume Arena 7.27.1 does not expose a
	/// mixer's FIRST parameter at all -- measured on genlock, wipe, relay,
	/// lenticular and pepper: it is absent from Arena's mixer panel and REST
	/// JSON, so it is stuck at its default. Whatever sits here must be a
	/// control whose default is right if nobody can ever reach it: Linear
	/// Light, on, which is how light adds. Arena, not the harness, must
	/// confirm which parameter it hides.
	enum ParamID : FFUInt32
	{
		//Light
		PT_LINEAR_LIGHT,
		PT_ROOM_LIGHT,

		//Occluder
		PT_OPACITY,///< the occluder's opacity: Arena binds it to the LAYER's fader
		PT_OCCLUDER,
		PT_DISTANCE,

		//Lamp 1, 2, 3: LAMP_FIELDS each
		PT_LAMP_FIRST,
		PT_LAMP_LAST = PT_LAMP_FIRST + penumbra::kLamps * LAMP_FIELDS - 1,

		//Screen
		PT_SCREEN,
		PT_VIEW,

		//About. Last in the enum so nothing before it ever moves.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	static constexpr FFUInt32 LampParam( int lamp, LampField field )
	{
		return PT_LAMP_FIRST + static_cast< FFUInt32 >( lamp ) * LAMP_FIELDS + field;
	}

private:
	penumbra::Asked asked() const;

	ffglex::FFGLShader convertShader;
	ffglex::FFGLShader prefixShader;
	ffglex::FFGLShader screenShader;
	ffglex::FFGLScreenQuad quad;
	penumbra::TableBuffer table[ 2 ];

	penumbra::Setup lastSetup;

	float params[ PT_COUNT ] = {};

	int fault                    = 0;
	const char* fragmentOverride = nullptr;

	//The host-session log: what reached the mixer. Nothing renders from it.
	unsigned long frames  = 0;
	int instance          = 0;
	int opacityLines      = 0;
	float loggedOpacity   = -1.0f;
	unsigned guardsLogged = 0;
	int loggedTableW      = 0;
	int loggedTableH      = 0;
	void LogGuard( unsigned bit, const char* what );

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
