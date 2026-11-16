#pragma once

#include <FFGLSDK.h>

namespace penumbra
{

/**
    An R32UI texture and the framebuffer that draws into it: one half of the
    summed-area table's ping-pong.

    Its own class, not throw's `PassBuffer`, because `FFGLFBO` cannot make
    one: `GenerateColorTexture` calls `glTexImage2D( ..., GL_RGBA,
    GL_UNSIGNED_BYTE or GL_FLOAT )`, and an integer internal format needs
    `GL_RED_INTEGER` with `GL_UNSIGNED_INT` -- the GL rejects the mix with
    GL_INVALID_OPERATION and leaves no texture.

    The filters are set to NEAREST, and that is load-bearing: an integer
    texture whose minification filter is anything else -- including the
    default, NEAREST_MIPMAP_LINEAR -- is INCOMPLETE (OpenGL 4.1 core,
    3.8.14), and fetching from an incomplete texture returns nothing useful.
    R32UI itself is a required texture and renderbuffer format (3.8.3), so
    every GL 4.1 driver can render into it.

    `Ensure` reallocates only when the size changes, and puts back the
    texture and framebuffer bindings it touched. Call it before anything is
    bound for the frame.
*/
class TableBuffer
{
public:
	TableBuffer() = default;
	~TableBuffer() = default;
	TableBuffer( const TableBuffer& ) = delete;
	TableBuffer& operator=( const TableBuffer& ) = delete;

	/// Make it `width` x `height` (texels), keeping it if it already is.
	/// False if the GL would not make it, with nothing left allocated.
	bool Ensure( int width, int height );

	/// Free both GL objects. Safe to call twice.
	void Destroy();

	GLuint Texture() const
	{
		return texture;
	}
	GLuint Framebuffer() const
	{
		return fbo;
	}
	int Width() const
	{
		return width;
	}
	int Height() const
	{
		return height;
	}
	bool Ready() const
	{
		return texture != 0 && fbo != 0;
	}

private:
	GLuint texture = 0;
	GLuint fbo     = 0;
	int width      = 0;
	int height     = 0;
};

} // namespace penumbra
