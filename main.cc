/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*- */
/*
 * main.cc
 * Copyright (C) Adam 2012 <aherbst@localhost.localdomain>
 * 
 * testproj is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * testproj is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

//networking
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/errno.h>
//GUI
#include <gtkmm/window.h>
#include <gtkmm/main.h>
//PortMIDI
#include <portmidi.h>
#include <porttime.h>
//multi-threading
#include <boost/thread.hpp>
//IO and other standard includes
#include <iostream>
#include <cstring>
#include <unordered_map>

using std::cout;
using std::cerr;
using std::endl;
using std::pair;
using std::unordered_map;
using std::out_of_range;

#define EVENT_BUFFER_SIZE 20 //max # of MIDI events allowed in the buffer
#define MAX_KEYS 100 //upper bound for # of MIDI notes mapped to keyboard keys
#define BASE_NOTE 40 //what MIDI note is played when you press 'Z' on the keyboard - increments as you move across-then-up
#define BUFSIZE 1024 //upper bound on packet size sent to/from network socket
#define MAX_MIDI_MSG_SIZE 24 //a MIDI message is 3 bytes: byte 1 = event type (note on/off) and channel, byte 2 = note, byte 3 = velocity

//network data
int sockfd, localfd;
char midiMsg[MAX_MIDI_MSG_SIZE];
char readbuf[BUFSIZE], msgbuf[BUFSIZE], writebuf[BUFSIZE];

//MIDI data
const PmDeviceInfo *device, *input, *output;
PmStream *inStream, *outStream;
PmError error;
void *time_info;
PtError timeError;
PmEvent eventBuffer[EVENT_BUFFER_SIZE];

//computer keyboard data
char keys[MAX_KEYS];
int numKeys;
bool keyDown[MAX_KEYS]; //whether each key is currently pressed
unordered_map<int, int> keyInd; //keyInd[c] = index of char c in keys array

//function declarations - defined below main
void sendMsg(int sockfd, const char *message);
void recvMsg(int sockfd);
bool keyPress(GdkEventKey *key);
bool keyRelease(GdkEventKey *key);
PmError check(PmError error);
void monitorSocket();

int main(int argc, char *argv[])
{
	//network setup
	bool server = false;
	cout << argc << " arguments: ";
	for(int i = 0; i < argc; i++) cout << argv[i] << ' ';
	cout << endl;
	if(argc > 1) server = atoi(argv[1])>0;

	cout << "Running as " << (server ? "server" : "client") << " with buffer size " << BUFSIZE << endl;
	
	int status;
	struct addrinfo hints, *res, *p;
	char ipstr[INET6_ADDRSTRLEN];
	struct sockaddr_storage remote_addr;
	socklen_t remote_addrlen;

	char *message;
	int bytesSent;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	const char *address = (server ? "localhost" : "localhost");
	if((status = getaddrinfo(address, "3490", &hints, &res)) != 0) {
		cerr << "getaddrinfo error: " << gai_strerror(status) << endl;
		exit(1);
	}

    for(p = res; p != NULL; p = p->ai_next) {
        void *addr;
        char *ipver;
		short port;

        // get the pointer to the address itself,
        // different fields in IPv4 and IPv6:
        if (p->ai_family == AF_INET) { // IPv4
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
            addr = &(ipv4->sin_addr);
			port = ipv4->sin_port;
            ipver = "IPv4";
        } else { // IPv6
            struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)p->ai_addr;
            addr = &(ipv6->sin6_addr);
			port = ipv6->sin6_port;
            ipver = "IPv6";
        }

        // convert the IP to a string and print it:
        inet_ntop(p->ai_family, addr, ipstr, sizeof ipstr);
		cout << "  " << ipver << ": " << ipstr << " on port " << htons(port) << endl;
    }

	if((sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol)) < 0) {
		perror("socket");
		exit(1);
	}
	else if(server) localfd = sockfd;
	if(server) {
		if(bind(localfd, res->ai_addr, res->ai_addrlen) < 0) {
			perror("bind");
			exit(1);
		}
		if(listen(localfd, 5) < 0) {
			perror("listen");
			exit(1);
		}
		if((sockfd = accept(localfd, (struct sockaddr *)&remote_addr, &remote_addrlen)) < 0) {
			perror("accept");
			exit(1);
		}
		recvMsg(sockfd);
		cout << "Received " << readbuf << endl;
		message = (char *)malloc(strlen(readbuf) * sizeof(char));
		strcpy(message, readbuf);
		sendMsg(sockfd, message);
		cout << "Sent " << message << endl;
	} else {
		if(connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
			perror("connect");
			exit(1);
		}
		message = "HELLO POODLE";
		sendMsg(sockfd, message);
		cout << "Sent " << writebuf << endl;
		recvMsg(sockfd);
		cout << "Received " << readbuf << endl;
	}
	
	//MIDI setup
	Pm_Initialize();

	int numDevices = Pm_CountDevices();
	//defaults from pmdefaults program
	PmDeviceID defInput, defOutput, deviceID;
	defInput = Pm_GetDefaultInputDeviceID();
	defOutput = Pm_GetDefaultOutputDeviceID();
		
	for(PmDeviceID i = 0; i < numDevices; i++) {
		device = Pm_GetDeviceInfo(i);
		cout << "DEVICE " << i << endl;
		cout << "  Interface: " << device->interf << endl;
		cout << "  Name:      " << device->name << endl;
		cout << "  Input:     " << (device->input ? "YES" : "NO") << (i == defInput ? " (default)" : "") << endl;
		cout << "  Output:    " << (device->output ? "YES" : "NO") << (i == defOutput ? " (default)" : "") << endl;
		if(i == 1) input = device; //if(i == defInput)
		if(i == 2) output = device; //if(i == defOutput)
	}

	timeError = Pt_Start(1, NULL, NULL);
	
	cout << "Opening stream to read from " << input->name << " and write to " << output->name << endl;
	error = Pm_OpenInput(&inStream, 1, NULL, EVENT_BUFFER_SIZE, NULL, time_info);
	error = Pm_OpenOutput(&outStream, 2, NULL, EVENT_BUFFER_SIZE, NULL, time_info, 1);

	eventBuffer[0].timestamp = 0;
	eventBuffer[0].message = Pm_Message(145, 60, 90);
	eventBuffer[1].timestamp = 1000;
	eventBuffer[1].message = Pm_Message(129, 60, 90);

	error = Pm_Write(outStream, eventBuffer, 2);

	strcpy(keys, "zxcvbnm,./asdfghjkl;'qwertyuiop[]1234567890-=");
	numKeys = strlen(keys);
	for(int i = 0; i < numKeys; i++) {
		keyDown[i] = FALSE;
		int keyInt = (int)keys[i];
		cout << "Inserting pair <" << keyInt << "," << i << ">" << endl;
		pair<int,int> keyPair(keyInt,i);
		keyInd.insert(keyPair);
	}

	//create a thread to listen for network events
	boost::thread socketListener(monitorSocket);

	//create the GUI window
	Gtk::Main kit(argc, argv);
	Gtk::Window window;

	window.add_events(Gdk::KEY_PRESS_MASK|Gdk::KEY_RELEASE_MASK);
	window.signal_key_press_event().connect(sigc::ptr_fun(&keyPress));
	window.signal_key_release_event().connect(sigc::ptr_fun(&keyRelease));

	window.set_title("gtkmm example window");
	window.set_default_size(400, 400);
	window.show_all();

	kit.run(window);

	error = Pm_Close(inStream);
	error = Pm_Close(outStream);

	Pm_Terminate();//*/

	if(server) close(localfd);
	close(sockfd);
	freeaddrinfo(res);

	//std::cout << "Hello world!" << std::endl;
	return 0;
}

//send a packet through the socket
void sendMsg(int sockfd, const char *message) {
	int bytesSent = 0, status;
	do {
		int bytesLeft = strlen(message) - bytesSent;
		strncpy(writebuf, &message[bytesSent], bytesLeft);
		writebuf[bytesLeft] = '\0';
		status = send(sockfd, writebuf, strlen(writebuf)+1, 0);
		if(status < 0) {
			perror("send");
			exit(1);
		}
		bytesSent += status;
	}while(bytesSent < strlen(message)+1);
}

//read all socket data when we are notified that a packet has arrived
void recvMsg(int sockfd) {
	int status;
	msgbuf[0] = '\0';
	do {
		status = read(sockfd, readbuf, BUFSIZE);
		if(status < 0) {
			perror("receive");
			exit(1);
		}
		strncat(msgbuf, readbuf, status);
	}while(readbuf[status-1] != '\0');	
}

//wrapper for running PortMIDI commands that may return an error
PmError check(PmError error) {
	if(error != pmNoError) {
		cout << "PM Error " << error << endl;
		exit(1);
	}
	return error;
}

//send MIDI note-on message corresponding to which key was pressed
bool keyPress(GdkEventKey *key) {
	int ind;
	try{
		ind = keyInd.at(key->keyval); //get the MIDI note associated with the pressed key
	}catch(out_of_range& oor) {
		cerr << "\tKey not mapped" << endl;
		return FALSE;
	}
	if(keyDown[ind]) return TRUE;
	cout << "KEY PRESS: " << key->keyval << " [" << ind << "]" << endl;
	keyDown[ind] = TRUE;

	//first play the MIDI note locally
	eventBuffer[0].timestamp = 0;
	eventBuffer[0].message = Pm_Message(145, BASE_NOTE+ind, 90);
	error = Pm_Write(outStream, eventBuffer, 1);

	//then send it over the socket to be played remotely
	midiMsg[0] = 145; //binary 1001-0001 means the event is note-on and the MIDI channel is 1
	midiMsg[1] = BASE_NOTE + ind; //note pitch
	midiMsg[2] = 90; //note velocity
	midiMsg[3] = '\0';
	sendMsg(sockfd, midiMsg);
	
	return TRUE;
}

//send MIDI note-off message corresponding to which key was released
bool keyRelease(GdkEventKey *key) {
	int ind;
	try{
		ind = keyInd.at(key->keyval);
	}catch(out_of_range& oor) {
		cerr << "\tKey not mapped" << endl; //get the MIDI note associated with the released key
		return FALSE;
	}
	if(!keyDown[ind]) return TRUE;
	cout << "KEY PRESS: " << key->keyval << " [" << ind << "]" << endl;
	keyDown[ind] = FALSE;

	//first stop the MIDI note locally
	eventBuffer[0].timestamp = 0;
	eventBuffer[0].message = Pm_Message(129, BASE_NOTE+ind, 90);
	error = Pm_Write(outStream, eventBuffer, 1);

	//then send it over the socket to be stopped remotely
	midiMsg[0] = 129; //binary 1000-0001 means the event is note-off and the MIDI channel is 1
	midiMsg[1] = BASE_NOTE + ind; //note pitch
	midiMsg[2] = 90; //note velocity
	midiMsg[3] = '\0';
	sendMsg(sockfd, midiMsg);
	
	return TRUE;
}

//catch and play all MIDI events sent to us through the socket
void monitorSocket() {
	do {
		recvMsg(sockfd); //blocks until data is received on the socket
		cout << "Received message: ";
		//output the bytes received on the socket for debugging
		for(int i = 0; i < strlen(msgbuf); i++) cout << (int)msgbuf[i] << ' ';
		cout << endl;
		//generate the MIDI event corresponding to the byte sequence
		eventBuffer[0].timestamp = 0;
		eventBuffer[0].message = Pm_Message(msgbuf[0], msgbuf[1], msgbuf[2]);
		error = Pm_Write(outStream, eventBuffer, 1);
	} while(msgbuf[1] != BASE_NOTE);
}
