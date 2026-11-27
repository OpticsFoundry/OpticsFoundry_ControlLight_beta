#ifndef TESTSEQUENCE_H
#define TESTSEQUENCE_H

#include "QTCPIP.h"

bool InitializeSequencer();
bool ResetSystem();
bool ExecuteTestSequence();
bool CycleSequence();
bool CycleSequenceWithIndividualCommandUpdate();
bool StopCyclingButtonPressed();
bool ShowSequencerNetworkSettings();

#endif // TESTSEQUENCE_H
