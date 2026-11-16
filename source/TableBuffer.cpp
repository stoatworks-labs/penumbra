#include "TableBuffer.h"

namespace penumbra
{

bool TableBuffer::Ensure( int w, int h )
{
	if( Ready() && w == width && h == height )
		return true;
	Destroy();
	if( w <= 0 || h <= 0 )
		return false;

	GLint maxSize = 0;
	glGetIntegerv( GL_MAX_TEXTURE_SIZE, &maxSize );
	if( w > maxSize || h > maxSize )
		return false;

	GLint oldTexture = 0, oldDraw = 0, oldRead = 0;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &oldTexture );
	glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &oldDraw );
	glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &oldRead );

	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R32UI, w, h, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	const bool complete = glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;

	glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( oldTexture ) );
	glBindFramebuffer( GL_DRAW_FRAMEBUFFER, static_cast< GLuint >( oldDraw ) );
	glBindFramebuffer( GL_READ_FRAMEBUFFER, static_cast< GLuint >( oldRead ) );

	if( !complete )
	{
		Destroy();
		return false;
	}
	width  = w;
	height = h;
	return true;
}

void TableBuffer::Destroy()
{
	if( fbo != 0 )
		glDeleteFramebuffers( 1, &fbo );
	if( texture != 0 )
		glDeleteTextures( 1, &texture );
	fbo     = 0;
	texture = 0;
	width   = 0;
	height  = 0;
}

} // namespace penumbra
