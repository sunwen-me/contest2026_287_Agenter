/*
 * SPDX-License-Identifier: Apache-2.0
 */

interface IVelaRosRuntime {
    int getState();
    int getBridgeEnabled();
    int getDomainId();
    int getParticipantId();
    int getHeartbeatPeriodMs();
    void setBridgeEnabled(int enabled);
    void setDomainId(int domainId);
    void setParticipantId(int participantId);
    void setHeartbeatPeriodMs(int periodMs);
    int getRequestCount();
    void shutdown();
}
