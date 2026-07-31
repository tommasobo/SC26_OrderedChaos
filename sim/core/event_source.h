#pragma once

#include <string>

#include "eventlist.h"
#include "logger_types.h"

class EventSource : public Logged {
public:
    EventSource(EventList& eventlist, const std::string& name)
        : Logged(name), _eventlist(eventlist){};

    EventSource(const std::string& name) : EventSource(EventList::getTheEventList(), name) {}

    virtual ~EventSource(){};
    virtual void doNextEvent() = 0;

    inline EventList& eventlist() const { return _eventlist; }

protected:
    EventList& _eventlist;
};