#include "atlahs_htsim_api.h"
#include "atlahs_event.h"
#include "fat_tree_topology.h"
#include "logsim-interface.h"
#include "lgs/LogGOPSim.hpp"
    

void AtlahsHtsimApi::Send(const SendEvent &event, graph_node_properties elem) {
    //std::cout << "AtlahsHtsimApi: Sending event" << std::endl;
    cleanup_finished_flows();


    // New additions
    /* UecSrc::_rss_params                   = {8, timeFromUs(200.), UecSrc::MEAN_RTT, 3, 0, 0, 25};
    UecSrc::_flowbender_params            = {0.05, 1};
    UecSrc::_uss_params                   = {8, 3};
    UecSrc::ecmp_background_traffic_nodes = 0;
    UecSrc::_load_balancing_algo = UecSrc::RSS; */
    int to = event.getTo();
    int from = event.getFrom();
    int tag = event.getTag();
    int size = event.getSizeBytes();
    size = size * 1;

    simtime_picosec transmission_delay =
            (Packet::data_packet_size() * 8 / speedAsGbps(linkspeed) * _topo->get_diameter() *
             1000) +
            (UecBasePacket::get_ack_size() * 8 / speedAsGbps(linkspeed) * _topo->get_diameter() *
             1000);
        simtime_picosec base_rtt_bw_two_points =
            2 * _topo->get_two_point_diameter_latency(from, to) + transmission_delay;

    

    if (getNumberNic() > 1) {
        from = getHtsimNodeNumber(from, elem.nic);
        to = getHtsimNodeNumber(to, elem.nic);
    }

    if (from == to) {
        std::cerr << "Error: Send event from and to the same node" << std::endl;
        exit(0);
    }

    /* printf("Creating Connection from %d to %d with size %d and tag %d\n", from, to, size, tag);
    fflush(stdout); */

    if (_logsim_interface->get_protocol() == UEC_PROTOCOL) { 

        //UecSrc *uecSrc = new UecSrc(NULL, NULL, *_eventlist, getSenderRtt(), getSenderBdp(), 100, 6);
        TrafficLoggerSimple* traffic_logger = NULL;
        auto uecSrc = std::make_unique<UecSrc>(traffic_logger, *_eventlist, *uec_nics.at(from), 1);

        uecSrc->setFlowSize(size);
        uecSrc->initNscc(cwnd_b, base_rtt_bw_two_points);


        uecSrc->setName("uec_" + std::to_string(from) + "_" + std::to_string(to));
        uecSrc->from = from;
        uecSrc->to = to;
        uecSrc->tag = tag;
        uecSrc->send_size = size;
        uecSrc->_atlahs_api = this;

        auto uecSink = std::make_unique<UecSink>(traffic_logger,
                                                 linkspeed,
                                                 1.1,
                                                 UecBasePacket::unquantize(UecSink::_credit_per_pull),
                                                 *_eventlist,
                                                 *uec_nics.at(to),
                                                 1);
        uecSink->setName("uec_sink_Rand");
        uecSink->from_sink = from;
        uecSink->to_sink = to;
        uecSink->tag_sink = tag;

        uecSrc->set_dst(to);
        uecSrc->setSrc(from);
        uecSrc->setDst(to);
        uecSink->set_src(from);

        auto srctotor = std::make_unique<Route>();
        srctotor->push_back(_topo->queues_ns_nlp[from][_topo->HOST_POD_SWITCH(from)][0]);
        srctotor->push_back(_topo->pipes_ns_nlp[from][_topo->HOST_POD_SWITCH(from)][0]);
        srctotor->push_back(_topo->queues_ns_nlp[from][_topo->HOST_POD_SWITCH(from)][0]->getRemoteEndpoint());

        auto dsttotor = std::make_unique<Route>();
        dsttotor->push_back(_topo->queues_ns_nlp[to][_topo->HOST_POD_SWITCH(to)][0]);
        dsttotor->push_back(_topo->pipes_ns_nlp[to][_topo->HOST_POD_SWITCH(to)][0]);
        dsttotor->push_back(_topo->queues_ns_nlp[to][_topo->HOST_POD_SWITCH(to)][0]->getRemoteEndpoint());

        //graph_node_properties* node_copy = new graph_node_properties(elem);
        uecSrc->lgs_node = std::make_unique<graph_node_properties>(elem);
        //uecSrc->connect(srctotor, dsttotor, *uecSink, _eventlist->now());

        uecSrc->connectPort(0, *srctotor, *dsttotor, *uecSink, _eventlist->now());
        flowid_t src_flow_id  = uecSrc->flowId();
        flowid_t sink_flow_id = uecSink->flowId();

        //register src and snk to receive packets from their respective TORs. 
        assert(_topo->switches_lp[_topo->HOST_POD_SWITCH(from)]);
        assert(_topo->switches_lp[_topo->HOST_POD_SWITCH(from)]);
        _topo->switches_lp[_topo->HOST_POD_SWITCH(from)]->addHostPort(
                        from, sink_flow_id, uecSrc->getPort(0));
        _topo->switches_lp[_topo->HOST_POD_SWITCH(to)]->addHostPort(
                        to, src_flow_id, uecSink->getPort(0));

        FlowHolder holder;
        holder.from         = from;
        holder.to           = to;
        holder.src_flow_id  = src_flow_id;
        holder.sink_flow_id = sink_flow_id;
        holder.src          = std::move(uecSrc);
        holder.sink         = std::move(uecSink);
        holder.srctotor     = std::move(srctotor);
        holder.dsttotor     = std::move(dsttotor);
        _active_flows[src_flow_id] = std::move(holder);

    }
    // TODO: Move this stuff to a CreateConnection function inside UEC. 
    // TODO: Support different tranports, not just UEC
}

void AtlahsHtsimApi::Recv(const RecvEvent &event) {
    // No Op for HTSIM
}

void AtlahsHtsimApi::Calc(const ComputeAtlahsEvent &event) {
    // Done Directly in lgs_interface for now
}

void AtlahsHtsimApi::Setup() {
    printf("No of nodes %d\n", total_nodes);

    if (_logsim_interface->get_protocol() == EQDS_PROTOCOL) {
        /* for (size_t ix = 0; ix < total_nodes; ix++){
            printf("Setting up node %d\n", ix);
            pacersEQDS.push_back(new EqdsPullPacer(linkspeed, 0.99, EqdsSrc::_mtu, *_eventlist));   
            nics.push_back(new EqdsNIC(*_eventlist, linkspeed));
        } */
    } else if (_logsim_interface->get_protocol() == NDP_PROTOCOL) {
        /* for (size_t ix = 0; ix < total_nodes; ix++)
            pacersNDP.push_back(new NdpPullPacer(*_eventlist,  linkspeed, 0.99));    */
    }


    for (size_t ix = 0; ix < total_nodes; ix++) {
        uec_pacers.push_back(new UecPullPacer(linkspeed,
                                          0.99,
                                          UecBasePacket::unquantize(UecSink::_credit_per_pull),
                                          *_eventlist,
                                          1));

        UecNIC* nic = new UecNIC(ix, *_eventlist, linkspeed, 1);
        uec_nics.push_back(nic);
    }
    
}

void AtlahsHtsimApi::EventFinished(const EventOver &event) {
    //std::cout << "AtlahsHtsimApi: Event is over" << std::endl;
    cleanup_finished_flows();

    if (AtlahsEventType::SEND_EVENT_OVER == event.getEventType()) {
        //_logsim_interface->flow_over(*(event.getPacket()));
        _logsim_interface->flow_over(event);
        _finished_flows.push_back(FinishedFlow{event.flow_id, _eventlist->now()});
        count_flows++;
        if (finish_after > 0 && count_flows >= finish_after) {
            _logsim_interface->force_finish = true;
        }

    } else if (AtlahsEventType::COMPUTE_EVENT_OVER == event.getEventType()) {
        _logsim_interface->compute_over(1);
    } else {
        abort();
    }
}

void AtlahsHtsimApi::cleanup_finished_flows() {
    if (_finished_flows.empty()) {
        return;
    }

    const simtime_picosec cleanup_grace_period = timeFromUs((uint32_t)200);
    std::vector<FinishedFlow> pending;
    pending.reserve(_finished_flows.size());

    for (const FinishedFlow& finished_flow : _finished_flows) {
        if ((_eventlist->now() - finished_flow.finished_at) < cleanup_grace_period) {
            pending.push_back(finished_flow);
            continue;
        }

        auto it = _active_flows.find(finished_flow.flow_id);
        if (it == _active_flows.end()) {
            continue;
        }

        const FlowHolder& flow = it->second;
        if (_topo != nullptr) {
            assert(_topo->switches_lp[_topo->HOST_POD_SWITCH(flow.from)]);
            assert(_topo->switches_lp[_topo->HOST_POD_SWITCH(flow.to)]);
            _topo->switches_lp[_topo->HOST_POD_SWITCH(flow.from)]->removeHostPort(
                    flow.from, flow.sink_flow_id);
            _topo->switches_lp[_topo->HOST_POD_SWITCH(flow.to)]->removeHostPort(
                    flow.to, flow.src_flow_id);
        }

        _active_flows.erase(it);
    }
    _finished_flows.swap(pending);
}
