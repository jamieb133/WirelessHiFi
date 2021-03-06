/**
 * @file AlsaWorker.cpp
 * @author Jamie Brown
 * @brief 
 * @version 0.1
 * @date 2019-02-17
 * 
 * @copyright Copyright (c) 2019
 * 
 */

#include "AlsaWorker.h"

#include <QtCore>
#include <iostream>
#include <unistd.h>

#define MTU 1500 //TODO: is there a way to query this from JACK at runtime?
#define XOVERFREQ 100 //TODO: what is the crossover frequency for the drivers/cabinet?
#define TAPS 100 //TODO: how mnay can we get away with?
#define PCMDEVICE "hw:0"

using namespace std;

extern QWaitCondition notify;

AlsaWorker::AlsaWorker(QtJack::Client& client, SlaveProcessor* processor)
{  
	m_processor = processor;
    m_buffer = new QtJack::AudioBuffer;
    m_alsaBuffer = new int64_t[m_processor->bufferSize()];
    m_dac = new AlsaController(client, PCMDEVICE);

    double xoverFreq = XOVERFREQ / static_cast<double>(client.sampleRate()); //normalise to nyquist

    firWoof = new CrossoverFilter(LOWPASS, xoverFreq, TAPS);
    firTweet = new CrossoverFilter(HIGHPASS, xoverFreq, TAPS);
}

void AlsaWorker::Work()
{
    cout << "WORKER: buffer size is " << m_processor->bufferSize() << endl;
    cout << "WORKER: sample-rate is " << m_processor->sampleRate() << endl;

    bool readOkay;
    bool packetDropped = true;
    bool streamRecovered = false;
    int64_t currentSample;
    int64_t leftSample32 = 0;
    int64_t rightSample32 = 0;
    double inputSample, leftSample, rightSample;
    int posPacket = 0;

    int samplesPerPacket = MTU / (m_processor->bitDepth() / 8);
    if (samplesPerPacket > 256) { samplesPerPacket = 256; } else { samplesPerPacket = 512; }
    int packetsPerBuffer = m_processor->bufferSize() / samplesPerPacket;

    /* wait until receiver thread is ready */
    m_mutex.lock();
    notify.wait(&m_mutex);
    m_mutex.unlock();

    while(true)
    {     
        m_mutex.lock();

        /* TODO: only call if this thread catches up to jack thread */
        notify.wait(&m_mutex);

        for (int pos = 0; pos < m_processor->bufferSize(); pos++)
        {
            /* copy from ring buffer into local ALSA buffer */
            inputSample = m_processor->ringBuffer->front();
            m_processor->ringBuffer->pop_front(); //shift buffer

            /* unfiltered  */
            leftSample = inputSample; 
            rightSample = inputSample;

            /* apply crossover filters */
            leftSample = firWoof->filter(inputSample);
            rightSample = firTweet->filter(inputSample);
            
            /* interleave each channel into MSB and LSB */
            leftSample32 = static_cast<int64_t>(leftSample*0x10000000);
            leftSample32 = leftSample32 & 0x00000000FFFFFFFF;
            rightSample32 = static_cast<int64_t>(rightSample*0x10000000);
            rightSample32 = rightSample32 << 32;
            rightSample32 = rightSample32 & 0xFFFFFFFF00000000; //shift right sample to 32 MSB
            currentSample = rightSample32 | leftSample32; 
            m_alsaBuffer[pos] = currentSample;                          

            /* any non-zero samples in a potential packet indicate that it was delivered 
                not sure this is the best method... */
            if (inputSample != 0) packetDropped = false;

            /* have we reached the end of a UDP packet? */
            if (posPacket == samplesPerPacket)
            {
                if (packetDropped)
                {
                    /* TODO: interpolate */
                    
                    /* placeholder: repeat previous packet */
                    for (int cnt = 0; cnt < samplesPerPacket; cnt++)
                    {
                        //m_alsaBuffer[cnt + (pos - samplesPerPacket)] = m_alsaBuffer[cnt + (pos - (2 * samplesPerPacket))];
                    }
                    if ( streamRecovered ) cout << "ALSA WORKER: stream lost" << endl;
                    streamRecovered = false; 
                }
                else
                {
                    if ( !streamRecovered ) cout << "ALSA WORKER: stream recovered" << endl;
                    streamRecovered = true;
                }
        
                /* reset for next packet */
                packetDropped = true;
                posPacket = 0;
            }
            else
            {
                posPacket++;
            }

        }

        /* output through alsa */
        if ( !m_dac->WriteInterleaved(m_alsaBuffer) )
        {
            cout << "ALSA WORKER: failed to write to device" << endl;
            exit(1);
        }


        //cout << "ALSA WORKER: input sample " << inputSample << endl;
        //cout << "ALSA WORKER: output sample "<< currentSample << endl;
        
        m_mutex.unlock();
        qApp->processEvents();
    }

    
}
