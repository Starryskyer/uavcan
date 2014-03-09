/*
 * Copyright (C) 2014 Pavel Kirienko <pavel.kirienko@gmail.com>
 */

#include <gtest/gtest.h>
#include <uavcan/subscriber.hpp>
#include <uavcan/util/method_binder.hpp>
#include <uavcan/mavlink/Message.hpp>
#include "common.hpp"
#include "transport/can/iface_mock.hpp"


template <typename DataType>
struct SubscriptionListener
{
    typedef uavcan::ReceivedDataStructure<DataType> ReceivedDataStructure;

    struct ReceivedDataStructureCopy
    {
        uint64_t ts_monotonic;
        uint64_t ts_utc;
        uavcan::TransferType transfer_type;
        uavcan::TransferID transfer_id;
        uavcan::NodeID src_node_id;
        DataType msg;

        ReceivedDataStructureCopy(const ReceivedDataStructure& s)
        : ts_monotonic(s.getMonotonicTimestamp())
        , ts_utc(s.getUtcTimestamp())
        , transfer_type(s.getTransferType())
        , transfer_id(s.getTransferID())
        , src_node_id(s.getSrcNodeID())
        , msg(s)
        { }
    };

    std::vector<DataType> simple;
    std::vector<ReceivedDataStructureCopy> extended;

    void receiveExtended(ReceivedDataStructure& msg)
    {
        extended.push_back(msg);
    }

    void receiveSimple(DataType& msg)
    {
        simple.push_back(msg);
    }

    typedef SubscriptionListener<DataType> SelfType;
    typedef uavcan::MethodBinder<SelfType*, void (SelfType::*)(ReceivedDataStructure&)> ExtendedBinder;
    typedef uavcan::MethodBinder<SelfType*, void (SelfType::*)(DataType&)> SimpleBinder;

    ExtendedBinder bindExtended() { return ExtendedBinder(this, &SelfType::receiveExtended); }
    SimpleBinder bindSimple() { return SimpleBinder(this, &SelfType::receiveSimple); }
};


TEST(Subscriber, Basic)
{
    uavcan::PoolAllocator<uavcan::MemPoolBlockSize * 8, uavcan::MemPoolBlockSize> pool;
    uavcan::PoolManager<1> poolmgr;
    poolmgr.addPool(&pool);

    // Manual type registration - we can't rely on the GDTR state
    uavcan::GlobalDataTypeRegistry::instance().reset();
    uavcan::DefaultDataTypeRegistrator<uavcan::mavlink::Message> _registrator;

    SystemClockDriver clock_driver;
    CanDriverMock can_driver(2, clock_driver);

    uavcan::OutgoingTransferRegistry<8> out_trans_reg(poolmgr);

    uavcan::Scheduler sch(can_driver, poolmgr, clock_driver, out_trans_reg, uavcan::NodeID(1));

    typedef SubscriptionListener<uavcan::mavlink::Message> Listener;

    uavcan::Subscriber<uavcan::mavlink::Message, Listener::ExtendedBinder> sub_extended(sch, poolmgr);
    uavcan::Subscriber<uavcan::mavlink::Message, Listener::ExtendedBinder> sub_extended2(sch, poolmgr); // Not used
    uavcan::Subscriber<uavcan::mavlink::Message, Listener::SimpleBinder> sub_simple(sch, poolmgr);
    uavcan::Subscriber<uavcan::mavlink::Message, Listener::SimpleBinder> sub_simple2(sch, poolmgr);     // Not used

    // Null binder - will fail
    ASSERT_EQ(-1, sub_extended.start(Listener::ExtendedBinder(NULL, NULL)));

    Listener listener;

    /*
     * Message layout:
     * uint8 seq
     * uint8 sysid
     * uint8 compid
     * uint8 msgid
     * uint8[<256] payload
     */
    uavcan::mavlink::Message expected_msg;
    expected_msg.seq = 0x42;
    expected_msg.sysid = 0x72;
    expected_msg.compid = 0x08;
    expected_msg.msgid = 0xa5;
    expected_msg.payload = "Msg";

    const uint8_t transfer_payload[] = {0x42, 0x72, 0x08, 0xa5, 'M', 's', 'g'};

    /*
     * RxFrame generation
     */
    std::vector<uavcan::RxFrame> rx_frames;
    for (int i = 0; i < 4; i++)
    {
        uavcan::TransferType tt = (i & 1) ? uavcan::TransferTypeMessageUnicast : uavcan::TransferTypeMessageBroadcast;
        uavcan::NodeID dni = (tt == uavcan::TransferTypeMessageBroadcast) ?
            uavcan::NodeID::Broadcast : sch.getDispatcher().getSelfNodeID();
        // uint_fast16_t data_type_id, TransferType transfer_type, NodeID src_node_id, NodeID dst_node_id,
        // uint_fast8_t frame_index, TransferID transfer_id, bool last_frame
        uavcan::Frame frame(uavcan::mavlink::Message::DefaultDataTypeID, tt, uavcan::NodeID(i + 100), dni, 0, i, true);
        frame.setPayload(transfer_payload, 7);
        uavcan::RxFrame rx_frame(frame, clock_driver.getMonotonicMicroseconds(), clock_driver.getUtcMicroseconds(), 0);
        rx_frames.push_back(rx_frame);
    }

    /*
     * Reception
     */
    ASSERT_EQ(0, sch.getDispatcher().getNumMessageListeners());

    ASSERT_EQ(1, sub_extended.start(listener.bindExtended()));
    ASSERT_EQ(1, sub_extended2.start(listener.bindExtended()));
    ASSERT_EQ(1, sub_simple.start(listener.bindSimple()));
    ASSERT_EQ(1, sub_simple2.start(listener.bindSimple()));

    ASSERT_EQ(4, sch.getDispatcher().getNumMessageListeners());

    sub_extended2.stop();  // These are not used - making sure they aren't receiving anything
    sub_simple2.stop();

    ASSERT_EQ(2, sch.getDispatcher().getNumMessageListeners());

    for (unsigned int i = 0; i < rx_frames.size(); i++)
    {
        can_driver.ifaces[0].pushRx(rx_frames[i]);
        can_driver.ifaces[1].pushRx(rx_frames[i]);
    }

    ASSERT_LE(0, sch.spin(clock_driver.getMonotonicMicroseconds() + 10000));

    /*
     * Validation
     */
    ASSERT_EQ(listener.extended.size(), rx_frames.size());
    for (unsigned int i = 0; i < rx_frames.size(); i++)
    {
        const Listener::ReceivedDataStructureCopy s = listener.extended.at(i);
        ASSERT_TRUE(s.msg == expected_msg);
        ASSERT_EQ(rx_frames[i].getSrcNodeID(), s.src_node_id);
        ASSERT_EQ(rx_frames[i].getTransferID(), s.transfer_id);
        ASSERT_EQ(rx_frames[i].getTransferType(), s.transfer_type);
        ASSERT_EQ(rx_frames[i].getMonotonicTimestamp(), s.ts_monotonic);
    }

    ASSERT_EQ(listener.simple.size(), rx_frames.size());
    for (unsigned int i = 0; i < rx_frames.size(); i++)
    {
        ASSERT_TRUE(listener.simple.at(i) == expected_msg);
    }

    ASSERT_EQ(0, sub_extended.getFailureCount());
    ASSERT_EQ(0, sub_simple.getFailureCount());

    /*
     * Unregistration
     */
    ASSERT_EQ(2, sch.getDispatcher().getNumMessageListeners());

    sub_extended.stop();
    sub_extended2.stop();
    sub_simple.stop();
    sub_simple2.stop();

    ASSERT_EQ(0, sch.getDispatcher().getNumMessageListeners());
}


static void panickingSink(const uavcan::ReceivedDataStructure<uavcan::mavlink::Message>&)
{
    FAIL() << "I just went mad";
}


TEST(Subscriber, FailureCount)
{
    uavcan::PoolAllocator<uavcan::MemPoolBlockSize * 8, uavcan::MemPoolBlockSize> pool;
    uavcan::PoolManager<1> poolmgr;
    poolmgr.addPool(&pool);

    // Manual type registration - we can't rely on the GDTR state
    uavcan::GlobalDataTypeRegistry::instance().reset();
    uavcan::DefaultDataTypeRegistrator<uavcan::mavlink::Message> _registrator;

    SystemClockDriver clock_driver;
    CanDriverMock can_driver(2, clock_driver);

    uavcan::OutgoingTransferRegistry<8> out_trans_reg(poolmgr);

    uavcan::Scheduler sch(can_driver, poolmgr, clock_driver, out_trans_reg, uavcan::NodeID(1));

    {
        uavcan::Subscriber<uavcan::mavlink::Message> sub(sch, poolmgr);
        ASSERT_EQ(0, sch.getDispatcher().getNumMessageListeners());
        sub.start(panickingSink);
        ASSERT_EQ(1, sch.getDispatcher().getNumMessageListeners());

        ASSERT_EQ(0, sub.getFailureCount());

        for (int i = 0; i < 4; i++)
        {
            // uint_fast16_t data_type_id, TransferType transfer_type, NodeID src_node_id, NodeID dst_node_id,
            // uint_fast8_t frame_index, TransferID transfer_id, bool last_frame
            uavcan::Frame frame(uavcan::mavlink::Message::DefaultDataTypeID, uavcan::TransferTypeMessageBroadcast,
                                uavcan::NodeID(i + 100), uavcan::NodeID::Broadcast, 0, i, true);
            // No payload - broken transfer
            uavcan::RxFrame rx_frame(frame, clock_driver.getMonotonicMicroseconds(),
                                     clock_driver.getUtcMicroseconds(), 0);
            can_driver.ifaces[0].pushRx(rx_frame);
            can_driver.ifaces[1].pushRx(rx_frame);
        }

        ASSERT_LE(0, sch.spin(clock_driver.getMonotonicMicroseconds() + 10000));

        ASSERT_EQ(4, sub.getFailureCount());

        ASSERT_EQ(1, sch.getDispatcher().getNumMessageListeners()); // Still there
    }
    ASSERT_EQ(0, sch.getDispatcher().getNumMessageListeners());     // Removed
}
