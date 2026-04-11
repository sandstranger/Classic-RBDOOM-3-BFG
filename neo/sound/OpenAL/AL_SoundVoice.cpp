/*
===========================================================================
Doom 3 BFG Edition GPL Source Code
...
===========================================================================
*/
#pragma hdrstop
#include "precompiled.h"
#include "../snd_local.h"

extern idCVar s_skipHardwareSets;
extern idCVar s_debugHardware;

// OpenAL soft/Android usually behaves better with the real device rate,
// but this file no longer forces it into streaming logic.
static int SYSTEM_SAMPLE_RATE = 48000;
static float ONE_OVER_SYSTEM_SAMPLE_RATE = 1.0f / SYSTEM_SAMPLE_RATE;

#define STREAMING_BUFFERS 8

static void FillOpenALStreamingBuffer( idSoundSample_OpenAL* sample, int bufferNumber, ALuint alBuffer )
{
	if( sample == NULL || alIsBuffer( alBuffer ) == AL_FALSE )
	{
		return;
	}

	if( bufferNumber < 0 || bufferNumber >= sample->buffers.Num() )
	{
		return;
	}

	ALenum format = AL_FORMAT_MONO16;

	if( sample->format.basic.formatTag == idWaveFile::FORMAT_PCM || sample->format.basic.formatTag == idWaveFile::FORMAT_FLOAT )
	{
		if( sample->useavi )
		{
			const int bitsPerSample = sample->format.basic.bitsPerSample;

			switch( bitsPerSample )
			{
				case 8:
					format = ( sample->NumChannels() == 1 ) ? AL_FORMAT_MONO8 : AL_FORMAT_STEREO8;
					break;
				case 16:
					format = ( sample->NumChannels() == 1 ) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
					break;
				case 32:
					format = ( sample->NumChannels() == 1 ) ? AL_FORMAT_MONO_FLOAT32 : AL_FORMAT_STEREO_FLOAT32;
					break;
				default:
					format = ( sample->NumChannels() == 1 ) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
					break;
			}
		}
		else
		{
			format = ( sample->NumChannels() == 1 ) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
		}
	}
	else if( sample->format.basic.formatTag == idWaveFile::FORMAT_ADPCM )
	{
		format = ( sample->NumChannels() == 1 ) ? AL_FORMAT_MONO_MSADPCM_SOFT : AL_FORMAT_STEREO_MSADPCM_SOFT;
	}
	else if( sample->format.basic.formatTag == idWaveFile::FORMAT_XMA2 )
	{
		format = ( sample->NumChannels() == 1 ) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
	}
	else
	{
		format = ( sample->NumChannels() == 1 ) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
	}

	int rate = sample->SampleRate();
	if( sample->useavi )
	{
		rate = sample->format.basic.samplesPerSec;
	}

	if( sample->format.basic.formatTag == idWaveFile::FORMAT_ADPCM )
	{
		alBufferi( alBuffer, AL_UNPACK_BLOCK_ALIGNMENT_SOFT, sample->format.extra.adpcm.samplesPerBlock );
	}

	alBufferData(
			alBuffer,
			format,
			sample->buffers[bufferNumber].buffer,
			sample->buffers[bufferNumber].bufferSize,
			rate
	);
}

/*
========================
idSoundVoice_OpenAL::idSoundVoice_OpenAL
========================
*/
idSoundVoice_OpenAL::idSoundVoice_OpenAL()
		:
		triggered( false ),
		openalSource( 0 ), idSoundVoice()
{
}

/*
========================
idSoundVoice_OpenAL::~idSoundVoice_OpenAL
========================
*/
idSoundVoice_OpenAL::~idSoundVoice_OpenAL()
{
	DestroyInternal();
}

/*
========================
idSoundVoice_OpenAL::CompatibleFormat
========================
*/
bool idSoundVoice_OpenAL::CompatibleFormat( idSoundSample* s )
{
	if( alIsSource( openalSource ) == AL_TRUE )
	{
		return true;
	}
	return false;
}

/*
========================
idSoundVoice_OpenAL::Create
========================
*/
void idSoundVoice_OpenAL::Create( const idSoundSample* leadinSample_, const idSoundSample* loopingSample_, const int channel_ )
{
	if( IsPlaying() )
	{
		Stop();
		return;
	}

	triggered = false;
	openalStreamingOffset = 0;

	leadinSample = ( idSoundSample_OpenAL* )leadinSample_;
	loopingSample = ( idSoundSample_OpenAL* )loopingSample_;
	channel = channel_;

	if( alIsSource( openalSource ) == AL_TRUE && CompatibleFormat( ( idSoundSample_OpenAL* )leadinSample ) )
	{
		sampleRate = leadinSample->GetFormat().basic.samplesPerSec;
	}
	else
	{
		DestroyInternal();

		formatTag = leadinSample->GetFormat().basic.formatTag;
		numChannels = leadinSample->GetFormat().basic.numChannels;
		sampleRate = leadinSample->GetFormat().basic.samplesPerSec;

		CheckALErrors();

		alGenSources( 1, &openalSource );
		if( CheckALErrors() != AL_NO_ERROR )
		{
			return;
		}

		alSourcef( openalSource, AL_ROLLOFF_FACTOR, 0.0f );

		// Recreate streaming buffers once, keep them alive until DestroyInternal().
		if( ((idSoundSample_OpenAL*)leadinSample)->openalBuffer == 0 )
		{
			for( int i = 0; i < STREAMING_BUFFERS; i++ )
			{
				if( alIsBuffer( openalStreamingBuffer[i] ) == AL_TRUE )
				{
					alDeleteBuffers( 1, &openalStreamingBuffer[i] );
				}
				openalStreamingBuffer[i] = 0;
			}

			alGenBuffers( STREAMING_BUFFERS, openalStreamingBuffer );
			CheckALErrors();
		}

		if( s_debugHardware.GetBool() )
		{
			if( loopingSample == NULL || loopingSample == leadinSample )
			{
				idLib::Printf( "%dms: %i created for %s\n", Sys_Milliseconds(), openalSource, leadinSample ? leadinSample->GetName() : "<null>" );
			}
			else
			{
				idLib::Printf( "%dms: %i created for %s and %s\n", Sys_Milliseconds(), openalSource, leadinSample ? leadinSample->GetName() : "<null>", loopingSample ? loopingSample->GetName() : "<null>" );
			}
		}
	}

	sourceVoiceRate = sampleRate;

	alSourcei( openalSource, AL_SOURCE_RELATIVE, AL_TRUE );
	alSource3f( openalSource, AL_POSITION, 0.0f, 0.0f, 0.0f );

	// Do not set source orientation here; that is a listener-side state in OpenAL.
}

/*
========================
idSoundVoice_OpenAL::DestroyInternal
========================
*/
void idSoundVoice_OpenAL::DestroyInternal()
{
	if( alIsSource( openalSource ) == AL_TRUE )
	{
		alSourceStop( openalSource );
		alSourcei( openalSource, AL_BUFFER, 0 );
		alDeleteSources( 1, &openalSource );
		openalSource = 0;
		openalStreamingOffset = 0;
		hasVUMeter = false;
	}

	for( int i = 0; i < STREAMING_BUFFERS; i++ )
	{
		if( alIsBuffer( openalStreamingBuffer[i] ) == AL_TRUE )
		{
			alDeleteBuffers( 1, &openalStreamingBuffer[i] );
			openalStreamingBuffer[i] = 0;
		}
	}

	triggered = false;
}

/*
========================
idSoundVoice_OpenAL::Start
========================
*/
void idSoundVoice_OpenAL::Start( int offsetMS, int ssFlags )
{
	if( s_debugHardware.GetBool() )
	{
		idLib::Printf( "%dms: %i starting %s @ %dms\n", Sys_Milliseconds(), openalSource, leadinSample ? leadinSample->GetName() : "<null>", offsetMS );
	}

	if( !leadinSample )
	{
		return;
	}

	if( alIsSource( openalSource ) == AL_FALSE )
	{
		return;
	}

	if( leadinSample->IsDefault() && !leadinSample->useavi )
	{
		idLib::Warning( "Starting defaulted sound sample %s", leadinSample->GetName() );
	}

	bool flicker = ( ssFlags & SSF_NO_FLICKER ) == 0;
	if( flicker != hasVUMeter )
	{
		hasVUMeter = flicker;
	}

	assert( offsetMS >= 0 );
	int offsetSamples = MsecToSamples( offsetMS, leadinSample->SampleRate() );
	if( loopingSample == NULL && offsetSamples >= leadinSample->GetPlayLength() )
	{
		return;
	}

	RestartAt( offsetSamples );

	// Force playback start explicitly.
	alSourcePlay( openalSource );
	paused = false;
}

/*
========================
idSoundVoice_OpenAL::RestartAt
========================
*/
int idSoundVoice_OpenAL::RestartAt( int offsetSamples )
{
	offsetSamples &= ~127;

	idSoundSample_OpenAL* sample = ( idSoundSample_OpenAL* )leadinSample;
	if( offsetSamples >= leadinSample->GetPlayLength() )
	{
		if( loopingSample != NULL )
		{
			offsetSamples %= loopingSample->GetPlayLength();
			sample = ( idSoundSample_OpenAL* )loopingSample;
			triggered = true;
		}
		else
		{
			return 0;
		}
	}
	else
	{
		triggered = false;
	}

	int previousNumSamples = 0;
	for( int i = 0; i < sample->buffers.Num(); i++ )
	{
		if( sample->buffers[i].numSamples > sample->playBegin + offsetSamples )
		{
			return SubmitBuffer( sample, i, sample->playBegin + offsetSamples - previousNumSamples );
		}
		previousNumSamples = sample->buffers[i].numSamples;
	}

	return 0;
}

/*
========================
idSoundVoice_OpenAL::SubmitBuffer
========================
*/
int idSoundVoice_OpenAL::SubmitBuffer( idSoundSample_OpenAL* sample, int bufferNumber, int offset )
{
	if( sample == NULL || bufferNumber < 0 || bufferNumber >= sample->buffers.Num() )
	{
		return 0;
	}

	if( sample->openalBuffer > 0 )
	{
		alSourcei( openalSource, AL_BUFFER, sample->openalBuffer );
		alSourcei( openalSource, AL_LOOPING, ( sample == loopingSample && loopingSample != NULL ) ? AL_TRUE : AL_FALSE );
		return sample->totalBufferSize;
	}

	// Streaming path: keep the source non-looping and refill manually.
	alSourcei( openalSource, AL_LOOPING, AL_FALSE );

	// Prime the queue with as many chunks as possible.
	ALuint queued[STREAMING_BUFFERS];
	int queuedCount = 0;

	idSoundSample_OpenAL* currentSample = sample;
	int currentIndex = bufferNumber;

	while( queuedCount < STREAMING_BUFFERS && currentSample != NULL )
	{
		if( currentIndex >= currentSample->buffers.Num() )
		{
			if( currentSample == leadinSample && loopingSample != NULL )
			{
				currentSample = dynamic_cast<idSoundSample_OpenAL *>(loopingSample);
				currentIndex = 0;
				triggered = true;
				continue;
			}

			if( currentSample == loopingSample && loopingSample != NULL )
			{
				currentIndex = 0;
				continue;
			}

			break;
		}

		if( alIsBuffer( openalStreamingBuffer[queuedCount] ) == AL_FALSE )
		{
			alGenBuffers( 1, &openalStreamingBuffer[queuedCount] );
		}

		FillOpenALStreamingBuffer( currentSample, currentIndex, openalStreamingBuffer[queuedCount] );
		queued[queuedCount] = openalStreamingBuffer[queuedCount];
		queuedCount++;

		currentIndex++;

		if( currentSample == leadinSample && currentIndex >= currentSample->buffers.Num() && loopingSample != NULL )
		{
			currentSample = dynamic_cast<idSoundSample_OpenAL *>(loopingSample);
			currentIndex = 0;
			triggered = true;
		}
	}

	openalStreamingOffset = currentIndex;

	if( queuedCount > 0 )
	{
		alSourceQueueBuffers( openalSource, queuedCount, queued );

		// Start immediately if this source is not already playing.
		ALint state = AL_INITIAL;
		alGetSourcei( openalSource, AL_SOURCE_STATE, &state );
		if( state != AL_PLAYING )
		{
			alSourcePlay( openalSource );
		}

		return sample->buffers[bufferNumber].bufferSize;
	}

	return 0;
}

/*
========================
idSoundVoice_OpenAL::Update
========================
*/
bool idSoundVoice_OpenAL::Update()
{
	if( alIsSource( openalSource ) == AL_FALSE )
	{
		return false;
	}

	// No EFX / no direct filter / no auxiliary send routing here.
	// This is the stable Android path.

	// If this is a streamed voice, refill processed buffers.
	if( leadinSample != NULL && (( idSoundSample_OpenAL* )leadinSample)->openalBuffer == 0 )
	{
		ALint processed = 0;
		alGetSourcei( openalSource, AL_BUFFERS_PROCESSED, &processed );

		while( processed-- > 0 )
		{
			ALuint buf = 0;
			alSourceUnqueueBuffers( openalSource, 1, &buf );

			idSoundSample_OpenAL* currentSample = ( triggered && loopingSample != NULL ) ? ( idSoundSample_OpenAL* )loopingSample : ( idSoundSample_OpenAL* )leadinSample;
			if( currentSample == NULL )
			{
				continue;
			}

			if( openalStreamingOffset >= currentSample->buffers.Num() )
			{
				if( currentSample == leadinSample && loopingSample != NULL )
				{
					currentSample = ( idSoundSample_OpenAL* )loopingSample;
					openalStreamingOffset = 0;
					triggered = true;
				}
				else if( currentSample == loopingSample && loopingSample != NULL )
				{
					openalStreamingOffset = 0;
				}
				else
				{
					// Nothing more to queue, stop cleanly.
					alSourceStop( openalSource );
					break;
				}
			}

			if( currentSample != NULL && openalStreamingOffset < currentSample->buffers.Num() )
			{
				FillOpenALStreamingBuffer( currentSample, openalStreamingOffset, buf );
				alSourceQueueBuffers( openalSource, 1, &buf );
				openalStreamingOffset++;

				if( currentSample == leadinSample && openalStreamingOffset >= currentSample->buffers.Num() && loopingSample != NULL )
				{
					triggered = true;
					openalStreamingOffset = 0;
				}
				else if( currentSample == loopingSample && loopingSample != NULL && openalStreamingOffset >= currentSample->buffers.Num() )
				{
					openalStreamingOffset = 0;
				}
			}
		}
	}

	return true;
}

/*
========================
idSoundVoice_OpenAL::IsPlaying
========================
*/
bool idSoundVoice_OpenAL::IsPlaying()
{
	if( alIsSource( openalSource ) == AL_FALSE )
	{
		return false;
	}

	ALint state = AL_INITIAL;
	alGetSourcei( openalSource, AL_SOURCE_STATE, &state );
	return ( state == AL_PLAYING );
}

/*
========================
idSoundVoice_OpenAL::FlushSourceBuffers
========================
*/
void idSoundVoice_OpenAL::FlushSourceBuffers()
{
	if( alIsSource( openalSource ) == AL_TRUE )
	{
		alSourceStop( openalSource );
		alSourcei( openalSource, AL_BUFFER, 0 );

		ALint queued = 0;
		alGetSourcei( openalSource, AL_BUFFERS_QUEUED, &queued );
		while( queued-- > 0 )
		{
			ALuint buf = 0;
			alSourceUnqueueBuffers( openalSource, 1, &buf );
		}

		openalStreamingOffset = 0;
		triggered = false;
	}
}

/*
========================
idSoundVoice_OpenAL::Pause
========================
*/
void idSoundVoice_OpenAL::Pause()
{
	if( alIsSource( openalSource ) == AL_FALSE || paused )
	{
		return;
	}

	if( s_debugHardware.GetBool() )
	{
		idLib::Printf( "%dms: %i pausing %s\n", Sys_Milliseconds(), openalSource, leadinSample ? leadinSample->GetName() : "<null>" );
	}

	alSourcePause( openalSource );
	paused = true;
}

/*
========================
idSoundVoice_OpenAL::UnPause
========================
*/
void idSoundVoice_OpenAL::UnPause()
{
	if( alIsSource( openalSource ) == AL_FALSE || !paused )
	{
		return;
	}

	if( s_debugHardware.GetBool() )
	{
		idLib::Printf( "%dms: %i unpausing %s\n", Sys_Milliseconds(), openalSource, leadinSample ? leadinSample->GetName() : "<null>" );
	}

	alSourcePlay( openalSource );
	paused = false;
}

/*
========================
idSoundVoice_OpenAL::Stop
========================
*/
void idSoundVoice_OpenAL::Stop()
{
	if( alIsSource( openalSource ) == AL_FALSE )
	{
		return;
	}

	if( !paused )
	{
		if( s_debugHardware.GetBool() )
		{
			idLib::Printf( "%dms: %i stopping %s\n", Sys_Milliseconds(), openalSource, leadinSample ? leadinSample->GetName() : "<null>" );
		}

		alSourceStop( openalSource );
		alSourcei( openalSource, AL_BUFFER, 0 );
		paused = true;
	}
}

/*
========================
idSoundVoice_OpenAL::GetAmplitude
========================
*/
float idSoundVoice_OpenAL::GetAmplitude()
{
	return 1.0f;
}

/*
========================
idSoundVoice_OpenAL::SetSampleRate
========================
*/
void idSoundVoice_OpenAL::SetSampleRate( uint32 newSampleRate, uint32 operationSet )
{
}

/*
========================
idSoundVoice_OpenAL::OnBufferStart
========================
*/
void idSoundVoice_OpenAL::OnBufferStart( idSoundSample* sample, int bufferNumber )
{
	idSoundSample_OpenAL* nextSample = ( idSoundSample_OpenAL* )sample;
	int nextBuffer = bufferNumber + 1;

	if( nextBuffer == sample->GetBuffers().Num() )
	{
		if( sample == leadinSample )
		{
			if( loopingSample == NULL )
			{
				return;
			}
			nextSample = ( idSoundSample_OpenAL* )loopingSample;
		}
		nextBuffer = 0;
	}

	SubmitBuffer( nextSample, nextBuffer, 0 );
}

int idSoundVoice_OpenAL::GetPlayingTimestamp()
{
	float seconds = -1.0f;
	if( IsPlaying() )
	{
		alGetSourcef( openalSource, AL_SEC_OFFSET, &seconds );
	}

	return seconds >= 0 ? ( int )( seconds * 1000.0f ) : -1;
}