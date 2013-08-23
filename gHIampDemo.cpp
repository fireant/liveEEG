/* gHIampDemo.cpp : Defines the entry point for the console application.
 *
 * This application reads data from exactly one g.HIamp device and writes received data to a binary output file ("receivedData.bin" in the working directory).
 * This binary output file can be read by using MATLAB for example. The file consists of consecutive float values (4 bytes each) that are the measured values in microvolts from the devices.
 * A single scan consists of one measured value (sample) for each channel, one following the other. The file contains a number of those scans, one complete scan following the other.
 *
 * Copyright (c) 2011 by Guger Technologies OG
 */

#include "stdafx.h"

#include <afxwin.h>
#include <afxmt.h>
#include "ringbuffer.h"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <string>
#include <deque>
#include <time.h>
#include <math.h>

#include <sstream>
using namespace std;

#include <gHIamp.h>

#include <zmq.hpp>
#include <conio.h>

#include "hr_time.h"

//#pragma comment(lib, "gHIamp.lib")

//function prototypes
void ConfigureDevice(HANDLE hDevice);
void StartAcquisition();
void StopAcquisition();
unsigned int DoAcquisition(LPVOID pArg);
bool ReadData(float* destBuffer, int numberOfScans, int *errorCode, string *errorMessage);
string GetDeviceErrorMessage();
void PrintDeviceErrorMessage();

//main program constants
const int BUFFER_SIZE_SECONDS = 8;			//the size of the application buffer in seconds
const long NUM_SECONDS_RUNNING = 60;			//the number of seconds that the application should acquire data (after this time elapsed the application will be stopped)
const int QUEUE_SIZE = 4;					//the number of GT_GetData calls that will be queued during acquisition to avoid loss of data

//device configuration settings
LPSTR _deviceSerial = "HA-2011.09.06";		//specify the serial number of the device
const unsigned int SAMPLE_RATE_HZ = 4800;	//the sample rate in Hz (see documentation of the g.USBamp API for details on this value and the NUMBER_OF_SCANS!)
const unsigned int NUMBER_OF_SCANS = 32;	//the number of scans that should be received simultaneously (depending on the _sampleRate; see C-API documentation for this value!)
//const unsigned int NUMBER_OF_CHANNELS = 15;	//the number of channels that should be acquired from the device (must equal the size of the _channelsToAcquire array)
//const unsigned int SAMPLE_RATE_HZ = 256;	//the sample rate in Hz (see documentation of the g.USBamp API for details on this value and the NUMBER_OF_SCANS!)
//const unsigned int NUMBER_OF_SCANS = 1;	//the number of scans that should be received simultaneously (depending on the _sampleRate; see C-API documentation for this value!)
const unsigned int NUMBER_OF_CHANNELS = 64;	//the number of channels that should be acquired from the device (must equal the size of the _channelsToAcquire array)
const BOOL ENABLE_TRIGGER = TRUE;			//true to include the triggers in data acquisition; otherwise, false

//global variables
HANDLE _hDevice;						//the handle to the device
CWinThread* _dataAcquisitionThread;		//the thread that performs data acquisition
bool _isRunning;						//flag that indicates if the thread is currently running
CMutex _bufferLock;						//mutex used to manage concurrent thread access to the application buffer
CRingBuffer<float> _buffer;				//the application buffer where received data will be stored for each device
CEvent _newDataAvailable;				//event to avoid polling the application data buffer for new data
CEvent _dataAcquisitionStopped;			//event that signals that data acquisition thread has been stopped
bool _bufferOverrun;					//flag indicating if an overrun occured at the application buffer


//this is the main entry point of the application
void main(int argc, _TCHAR* argv[])
{
	zmq::context_t context(1);
	zmq::socket_t publisher(context, ZMQ_PUB);
	publisher.bind("tcp://192.168.56.101:5556");

	CStopWatch timer;

	try
	{
		cout << "Opening device '" << _deviceSerial << "'...\n";

		//open device
		_hDevice = GT_OpenDevice(0);
		//_hDevice = GT_OpenDeviceEx(_deviceSerial);

		if (_hDevice == NULL)
			throw string("Error on GT_OpenDeviceEx: couldn't open device ").append(_deviceSerial);

		cout << "Configuring device...\n";

		int nossr = 0;

		int nof;
		GT_GetNumberOfFilter(&nof);
		cout<<"nof: "<<nof<<endl;

		FILT* flt = new FILT[nof];
		GT_GetFilterSpec(flt);

		for (int i=0; i< nof; i++) {
			cout<<i<<":"<<flt[i].fu<<":"<<flt[i].fo<<":"<<flt[i].fs<<":"<<flt[i].type<<endl;
		}
	
		//GT_GetNumberOfSupportedSampleRates(_hDevice, &nossr);
		//cout<<"nossr: "<<nossr<<endl;
		/*GT_GetNumberOfSupportedSampleRates(_hDevice, &nossr);

		float* ssr = new float[nossr];
		GT_GetSupportedSampleRates(_hDevice, ssr);
		for (int i=0; i<nossr; i++)
			cout<<ssr[i]<<endl;
		*/
		
		GT_Stop(_hDevice);
		GT_ResetTransfer(_hDevice);

		//configure device
		ConfigureDevice(_hDevice);
		

		/*GT_HIAMP_CHANNEL_IMPEDANCES imp;
		for (int i = 0; i<256; i++) {
			imp.IsActiveElectrode[i]=FALSE;
			imp.Impedance[i]=0.0;
		}

		BOOL status = GT_GetImpedance(_hDevice, &imp);
		cout<<"imp: "<<status<<" : ";
		for (int i = 0; i<10; i++) {
			cout<<imp.Impedance[i]<<" ";
		}
		cout<<endl;
		Sleep(200);*/
		
		//Sleep(2000);
		
		
		//determine how many scans should be read and processed at once by the processing thread (not the acquisition thread!)
		int numScans = NUMBER_OF_SCANS;
		int numChannels = NUMBER_OF_CHANNELS + (int) ENABLE_TRIGGER;

		//initialize temporary data buffer where data from the application buffer will be copied out
		float *receivedData = new float[numScans * numChannels];

		try
		{
			//for receiving error messages from ReadData
			int errorCode;
			string errorMessage;

			//initialize file stream where received data should be written to
			//CFile outputFile; 

			//if (!outputFile.Open(_T("receivedData.bin"), CFile::modeCreate | CFile::modeWrite | CFile::typeBinary))
			//	throw string("Error on creating/opening output file: file 'receivedData.bin' couldn't be opened.");

			try
			{
				cout<<sizeof(float)<<" "<<sizeof(double)<<endl;
				cout << "Starting acquisition...\n";

				//start data acquisition
				StartAcquisition();

				//cout << "Receiving data for " << NUM_SECONDS_RUNNING << " seconds...\n";
				cout << "Receiving data ...\n";

				//to stop the application after a specified time, get start time
				long startTime = clock();
				long endTime = startTime + NUM_SECONDS_RUNNING * CLOCKS_PER_SEC;

				//this is the data processing thread; data received from the devices will be written out to a file here
				//while (clock() < endTime)
				timer.startTimer();
				while (!_kbhit())
				{
					//to release CPU resources wait until the acquisition thread tells that new data has been received and is available in the buffer.
					WaitForSingleObject(_newDataAvailable.m_hObject, 1000);

					while (_buffer.GetSize() >= numScans * numChannels)
					{
						//read data from the application buffer and stop application if buffer overrun
						if (!ReadData(receivedData, numScans, &errorCode, &errorMessage))
						{
							if (errorCode == 2)
								break;
							else
								throw errorMessage;
						}
						timer.stopTimer();
						//cout<<"t: "<<setprecision(12)<<timer.getElapsedTime()<<endl;
						
						//stringstream ss;
						//ss<<setprecision(9)<<timer.getElapsedTime()<<" ";


						//for (size_t i=0; i<numScans * numChannels; i++) {
						//	ss<<receivedData[i]<<" ";
						//}
						//zmq::message_t message(ss.str().size()+1);
						zmq::message_t message(sizeof(double) + sizeof(float)* numScans * numChannels);
						//memcpy(message.data(), ss.str().c_str(), ss.str().size()+1);
						double ct = timer.getElapsedTime();
						memcpy(message.data(), &ct, sizeof(double));
						memcpy((char *) (message.data())+sizeof(double), receivedData, sizeof(float)* numScans * numChannels);
						publisher.send(message);
						//timer.startTimer();
						//cout<<"m:"<<ss.str().c_str()<<endl;
						//write data to file
						//outputFile.Write(receivedData, numScans * numChannels * sizeof(float));
						//cout<<"d1: "<<receivedData[64]<<endl;
						//cout<<sizeof(float)<<endl;

					}
				}
			}
			catch (string& exception)
			{
				//an exception occured during data acquisition
				cout << "\t" << exception << "\n";

				//continue execution in every case to clean up allocated resources (since no finally-block exists)
			}

			// 
			//in every case, stop data acquisition and clean up allocated resources 
			//since no finally statement exists in c++ and we can't mix it with the C __finally statement, the clean-up code follows the try-catch block.
			//

			//stop data acquisition
			StopAcquisition();
			
					
			//close output file
			//outputFile.Close();
		}
		catch (string& exception)
		{
			//an exception occured
			cout << "\t" << exception << "\n";

			//continue execution in every case to clean up allocated resources (since no finally-block exists)
		}

		cout << "Closing device...\n";
		
		//close device
		if (!GT_CloseDevice(&_hDevice))
			cout << "Error on GT_CloseDevice: couldn't close device" << GetDeviceErrorMessage() << "\n";

		//free allocated resources
		delete [] receivedData;

		cout << "Clean up complete. Bye bye!" << "\n\n";
	}
	catch (string& exception)
	{
		//in case an error occured during opening and initialization, report it and stop execution
		cout << "\t" << exception << "\n\n";
	}
	
	publisher.close();
	cout << "Press ENTER to exit...";
	getchar();
}

//initializes the configuration structure
void ConfigureDevice(HANDLE hDevice)
{
	GT_HIAMP_CONFIGURATION *deviceConfiguration = new GT_HIAMP_CONFIGURATION();
	SecureZeroMemory(deviceConfiguration, sizeof(GT_HIAMP_CONFIGURATION));

	//retrieve current device configuration as base for modifications
	if (!GT_GetConfiguration(hDevice, deviceConfiguration))
	{
		delete deviceConfiguration;
		throw string("Error on GT_GetConfiguration: couldn't retrieve configuration from device ").append(GetDeviceErrorMessage());
	}

	//modify configuration
	deviceConfiguration->SampleRate = SAMPLE_RATE_HZ;
	deviceConfiguration->BufferSize = NUMBER_OF_SCANS;
	deviceConfiguration->TriggerLineEnabled = ENABLE_TRIGGER;
	deviceConfiguration->HoldEnabled = FALSE;
	deviceConfiguration->IsSlave = FALSE;
	deviceConfiguration->CounterEnabled = FALSE;
	deviceConfiguration->InternalSignalGenerator.Enabled = FALSE;
	deviceConfiguration->InternalSignalGenerator.WaveShape = WS_SQUARE;
	deviceConfiguration->InternalSignalGenerator.Frequency = 10;
	deviceConfiguration->InternalSignalGenerator.Amplitude = TESTSIGNAL_AMPLITUDE;
	deviceConfiguration->InternalSignalGenerator.Offset = TESTSIGNAL_OFFSET;

	for (int i = 0; i < MAX_NUMBER_OF_CHANNELS; i++)
	{
		if (deviceConfiguration->Channels[i].Available)
		{
			deviceConfiguration->Channels[i].ChannelNumber = (i + 1);
			deviceConfiguration->Channels[i].Acquire = (deviceConfiguration->Channels[i].ChannelNumber <= NUMBER_OF_CHANNELS);
			deviceConfiguration->Channels[i].BandpassFilterIndex = -1;
			//deviceConfiguration->Channels[i].BandpassFilterIndex = 15;
			deviceConfiguration->Channels[i].NotchFilterIndex = -1;
			deviceConfiguration->Channels[i].BipolarChannel = 0;
		}
	}

	//apply configuration
	if (!GT_SetConfiguration(hDevice, *deviceConfiguration))
	{
		delete deviceConfiguration;
		throw string("Error on GT_SetConfiguration: couldn't set configuration on device ").append(GetDeviceErrorMessage());
	}

	cout << "\tdevice configured.\n";

	//retrieve current device configuration as base for modifications
	if (!GT_GetConfiguration(hDevice, deviceConfiguration))
	{
		delete deviceConfiguration;
		throw string("Error on GT_GetConfiguration: couldn't retrieve configuration from device ").append(GetDeviceErrorMessage());
	}

	delete deviceConfiguration;
}

//Starts the thread that does the data acquisition.
void StartAcquisition()
{
	int numChannels = NUMBER_OF_CHANNELS + (int) ENABLE_TRIGGER;

	_isRunning = true;
	_bufferOverrun = false;

	//give main process (the data processing thread) high priority
	HANDLE hProcess = GetCurrentProcess();
	SetPriorityClass(hProcess, HIGH_PRIORITY_CLASS);

	//initialize application data buffer to the specified number of seconds
	_buffer.Initialize(BUFFER_SIZE_SECONDS * SAMPLE_RATE_HZ * numChannels);

	//reset event
	_dataAcquisitionStopped.ResetEvent();

	//create data acquisition thread with high priority
	_dataAcquisitionThread = AfxBeginThread(DoAcquisition, NULL, THREAD_PRIORITY_TIME_CRITICAL,0, CREATE_SUSPENDED);
	_dataAcquisitionThread->ResumeThread();
}

//Stops the data acquisition thread
void StopAcquisition()
{
	//tell thread to stop data acquisition
	_isRunning = false;

	//wait until the thread has stopped data acquisition
	DWORD ret = WaitForSingleObject(_dataAcquisitionStopped.m_hObject, 10000);

	//reset the main process (data processing thread) to normal priority
	HANDLE hProcess = GetCurrentProcess();
	SetPriorityClass(hProcess, NORMAL_PRIORITY_CLASS);
}

/*
 * Starts data acquisition and acquires data until StopAcquisition is called (i.e., the _isRunning flag is set to false)
 * Then the data acquisition will be stopped.
 */
unsigned int DoAcquisition(LPVOID pArgs)
{
	int queueIndex = 0;
	int numChannels = NUMBER_OF_CHANNELS + (int) ENABLE_TRIGGER;
	int nPoints = NUMBER_OF_SCANS * numChannels;
	DWORD numValidBytes = nPoints * sizeof(float);
	DWORD bufferSizeBytes = (DWORD) ceil(numValidBytes / (double) MAX_USB_PACKET_SIZE) * MAX_USB_PACKET_SIZE;
	int timeOutMilliseconds = (int) ceil(2000 * (NUMBER_OF_SCANS / (double) SAMPLE_RATE_HZ));
	DWORD numBytesReceived = 0;

	//create the temporary data buffers (the device will write data into those)
	BYTE** buffers = new BYTE*[QUEUE_SIZE];
	OVERLAPPED* overlapped = new OVERLAPPED[QUEUE_SIZE];

	__try
	{
		//for each data buffer allocate a number of numValidBytes bytes
		for (queueIndex = 0; queueIndex < QUEUE_SIZE; queueIndex++)
		{
			buffers[queueIndex] = new BYTE[bufferSizeBytes];
			SecureZeroMemory(&(overlapped[queueIndex]), sizeof(OVERLAPPED));

			//create a windows event handle that will be signaled when new data from the device has been received for each data buffer
			overlapped[queueIndex].hEvent = CreateEvent(NULL, false, false, NULL);
		}

		queueIndex = 0;

		//start device
		if (!GT_Start(_hDevice))
		{
			cout << "\tError on GT_Start: Couldn't start data acquisition of device.\n";
			PrintDeviceErrorMessage();
			return 0;
		}

		//queue up the first batch of transfer requests to prevent overflow on the device
		for (queueIndex = 0; queueIndex < QUEUE_SIZE; queueIndex++)
		{
			if (!GT_GetData(_hDevice, buffers[queueIndex], bufferSizeBytes, &overlapped[queueIndex]))
			{
				cout << "\tError on GT_GetData.\n";
				PrintDeviceErrorMessage();
				return 0;
			}
		}

		queueIndex = 0;

		//continouos data acquisition
		while (_isRunning)
		{
			//wait for notification from the system telling that new data is available
			if (WaitForSingleObject(overlapped[queueIndex].hEvent, 5000) == WAIT_TIMEOUT)
			{
				cout << "\tError on data transfer: timeout occured.\n";
				PrintDeviceErrorMessage();
				return 0;
			}

			//get number of actually received bytes...
			if (!GT_GetOverlappedResult(_hDevice, &overlapped[queueIndex], &numBytesReceived, false))
			{
				cout << "\tError on data transfer: couldn't receive number of transferred bytes (GT_GetOverlappedResult() returned FALSE; Windows error code: " << GetLastError() << ")\n";
				return 0;
			}

			//...and check if we lost something (number of received bytes must be equal to the previously allocated buffer size)
			if (numBytesReceived != numValidBytes)
			{
				cout << "\tError on data transfer: samples lost.\n";
				return 0;
			}

			//to store the received data into the application data buffer at once, lock it
			_bufferLock.Lock();

			__try
			{
				//if we are going to overrun on writing the received data into the buffer, set the appropriate flag; the reading thread will handle the overrun
				_bufferOverrun = (_buffer.GetFreeSize() < nPoints);

				//store received data in the correct order (that is scan-wise, where one scan includes all channels)
				_buffer.Write((float*) buffers[queueIndex], nPoints);
			}
			__finally
			{
				//release the previously acquired lock
				_bufferLock.Unlock();
			}

			//add new GetData call to the queue replacing the currently received one
			if (!GT_GetData(_hDevice, buffers[queueIndex], bufferSizeBytes, &overlapped[queueIndex]))
			{
				cout << "\tError on GT_GetData.\n";
				PrintDeviceErrorMessage();
				return 0;
			}

			//signal processing (main) thread that new data is available
			_newDataAvailable.SetEvent();
			
			//increment circular queueIndex to process the next queue at the next loop repitition (on overrun start at index 0 again)
			queueIndex = (queueIndex + 1) % QUEUE_SIZE;
		}
	}
	__finally
	{
		cout << "Stopping device and cleaning up..." << "\n";

		//stop device
		if (!GT_Stop(_hDevice))
		{
			cout << "\tError on GT_Stop: couldn't stop device.\n";
			PrintDeviceErrorMessage();
		}

		//clean up allocated resources
		for (int i = 0; i < QUEUE_SIZE; i++)
		{
			if (WaitForSingleObject(overlapped[queueIndex].hEvent, timeOutMilliseconds) == WAIT_TIMEOUT)
				GT_ResetTransfer(_hDevice);

			CloseHandle(overlapped[queueIndex].hEvent);

			delete [] buffers[queueIndex];

			//increment queue index
			queueIndex = (queueIndex + 1) % QUEUE_SIZE;
		}

		delete [] buffers;
		delete [] overlapped;

		//reset _isRunning flag
		_isRunning = false;

		//signal event
		_dataAcquisitionStopped.SetEvent();

		//end thread
		AfxEndThread(0xdead);
	}

	return 0xdead;
}

/*
 * Reads the received numberOfScans scans from the device. If not enough data is available (errorCode == 2) or the application buffer overruns (errorCode == 1), this method returns false.
 * float* destBuffer:	the array that returns the received data from the application data buffer. 
						Data is aligned as follows: element at position destBuffer[scanIndex * numChannels + channelIndex] is sample of channel channelIndex (zero-based) of the scan with zero-based scanIndex.
						channelIndex ranges from 0..numChannelsPerDevices where numChannelsPerDevice the total number of channels of the device including the trigger line.
 * int numberOfScans:	the number of scans to retrieve from the application buffer.
 */
bool ReadData(float* destBuffer, int numberOfScans, int *errorCode, string *errorMessage)
{
	int numChannels = NUMBER_OF_CHANNELS + (int) ENABLE_TRIGGER;
	int validPoints = numChannels * numberOfScans;

	//wait until requested amount of data is ready
	if (_buffer.GetSize() < validPoints)
	{
		*errorCode = 2;
		*errorMessage = "Not enough data available";
		return false;
	}

	//acquire lock on the application buffer for reading
	_bufferLock.Lock();

	__try
	{
		//if buffer run over report error and reset buffer
		if (_bufferOverrun)
		{
			_buffer.Reset();
			*errorCode = 1;
			*errorMessage = "Error on reading data from the application data buffer: buffer overrun.";
			_bufferOverrun = false;
			return false;
		}

		//copy the data from the application buffer into the destination buffer
		_buffer.Read(destBuffer, validPoints);
	}
	__finally
	{
		_bufferLock.Unlock();
	}

	*errorCode = 0;
	*errorMessage = "No error occured.";
	return true;
}

string GetDeviceErrorMessage()
{
	WORD errorCode = 0;
	char errorMessage[256];

	if (!GT_GetLastError(&errorCode, errorMessage))
		return string("(reason unknown: error code could not be retrieved from device)");

	char exceptionMessage[512];
	sprintf_s(exceptionMessage, 512, "(#%d: %s)", errorCode, errorMessage);

	return string(exceptionMessage);
}

void PrintDeviceErrorMessage()
{
	cout << "\t" << GetDeviceErrorMessage() << "\n";
}
