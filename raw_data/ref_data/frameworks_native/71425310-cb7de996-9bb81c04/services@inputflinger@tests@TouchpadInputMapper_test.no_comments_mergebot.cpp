#include "TouchpadInputMapper.h"
#include <android-base/logging.h>
#include <gtest/gtest.h>
#include <com_android_input_flags.h>
#include <thread>
#include "FakePointerController.h"
#include "InputMapperTest.h"
#include "InterfaceMocks.h"
#include "TestInputListenerMatchers.h"
#include "TestEventMatchers.h"
#define TAG "TouchpadInputMapper_test"
namespace android {
using testing::Return;
using testing::VariantWith;
constexpr auto ACTION_DOWN = AMOTION_EVENT_ACTION_DOWN;
constexpr auto ACTION_UP = AMOTION_EVENT_ACTION_UP;
constexpr auto BUTTON_PRESS = AMOTION_EVENT_ACTION_BUTTON_PRESS;
constexpr auto BUTTON_RELEASE = AMOTION_EVENT_ACTION_BUTTON_RELEASE;
constexpr auto HOVER_MOVE = AMOTION_EVENT_ACTION_HOVER_MOVE;
constexpr auto HOVER_ENTER = AMOTION_EVENT_ACTION_HOVER_ENTER;
constexpr auto HOVER_EXIT = AMOTION_EVENT_ACTION_HOVER_EXIT;
constexpr int32_t DISPLAY_ID = 0;
constexpr int32_t DISPLAY_WIDTH = 480;
constexpr int32_t DISPLAY_HEIGHT = 800;
constexpr std::optional<uint8_t> NO_PORT = std::nullopt;
class TouchpadInputMapperTestBase : public InputMapperUnitTest {
protected:
    void SetUp() override {
        InputMapperUnitTest::SetUp();
        expectScanCodes( true,
                        {BTN_LEFT, BTN_RIGHT, BTN_TOOL_FINGER, BTN_TOOL_QUINTTAP, BTN_TOUCH,
                         BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP});
        expectScanCodes( false,
                        {BTN_TOOL_PEN, BTN_TOOL_RUBBER, BTN_TOOL_BRUSH, BTN_TOOL_PENCIL,
                         BTN_TOOL_AIRBRUSH});
        setScanCodeState(KeyState::UP, {BTN_TOUCH, BTN_STYLUS,
                                        BTN_STYLUS2, BTN_0,
                                        BTN_TOOL_FINGER, BTN_TOOL_PEN,
                                        BTN_TOOL_RUBBER, BTN_TOOL_BRUSH,
                                        BTN_TOOL_PENCIL, BTN_TOOL_AIRBRUSH,
                                        BTN_TOOL_MOUSE, BTN_TOOL_LENS,
                                        BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
                                        BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP,
                                        BTN_LEFT, BTN_RIGHT,
                                        BTN_MIDDLE, BTN_BACK,
                                        BTN_SIDE, BTN_FORWARD,
                                        BTN_EXTRA, BTN_TASK});
        setKeyCodeState(KeyState::UP,
                        {AKEYCODE_STYLUS_BUTTON_PRIMARY, AKEYCODE_STYLUS_BUTTON_SECONDARY});
        EXPECT_CALL(mMockEventHub,
                    mapKey(EVENTHUB_ID, BTN_LEFT, 0, 0, testing::_,
                           testing::_, testing::_))
                .WillRepeatedly(Return(NAME_NOT_FOUND));
        EXPECT_CALL(mMockEventHub, hasInputProperty(EVENTHUB_ID, INPUT_PROP_BUTTONPAD))
                .WillRepeatedly(Return(true));
        EXPECT_CALL(mMockEventHub, hasInputProperty(EVENTHUB_ID, INPUT_PROP_SEMI_MT))
                .WillRepeatedly(Return(false));
        setupAxis(ABS_MT_SLOT, true, 0, 4, 0);
        setupAxis(ABS_MT_POSITION_X, true, 0, 2000, 24);
        setupAxis(ABS_MT_POSITION_Y, true, 0, 1000, 24);
        setupAxis(ABS_MT_PRESSURE, true, 0, 255, 0);
        setupAxis(ABS_MT_ORIENTATION, false, 0, 0, 0);
        setupAxis(ABS_MT_TOUCH_MAJOR, false, 0, 0, 0);
        setupAxis(ABS_MT_TOUCH_MINOR, false, 0, 0, 0);
        setupAxis(ABS_MT_WIDTH_MAJOR, false, 0, 0, 0);
        setupAxis(ABS_MT_WIDTH_MINOR, false, 0, 0, 0);
        setupAxis(ABS_MT_TRACKING_ID, false, 0, 0, 0);
        setupAxis(ABS_MT_DISTANCE, false, 0, 0, 0);
        setupAxis(ABS_MT_TOOL_TYPE, false, 0, 0, 0);
        EXPECT_CALL(mMockEventHub, getAbsoluteAxisValue(EVENTHUB_ID, ABS_MT_SLOT, testing::_))
                .WillRepeatedly([](int32_t eventHubId, int32_t, int32_t* outValue) {
                    *outValue = 0;
                    return OK;
                });
        EXPECT_CALL(mMockEventHub, getMtSlotValues(EVENTHUB_ID, testing::_, testing::_))
                .WillRepeatedly([]() -> base::Result<std::vector<int32_t>> {
                    return base::ResultError("Axis not supported", NAME_NOT_FOUND);
                });
        createDevice();
        mMapper = createInputMapper<TouchpadInputMapper>(*mDeviceContext, mReaderConfiguration);
    }
};
class TouchpadInputMapperTest : public TouchpadInputMapperTestBase {
protected:
    void SetUp() override {
<<<<<<< HEAD
        input_flags::enable_pointer_choreographer(false);
        TouchpadInputMapperTestBase::SetUp();
|||||||
        InputMapperUnitTest::SetUp();
        expectScanCodes( true,
                        {BTN_LEFT, BTN_RIGHT, BTN_TOOL_FINGER, BTN_TOOL_QUINTTAP, BTN_TOUCH,
                         BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP});
        expectScanCodes( false,
                        {BTN_TOOL_PEN, BTN_TOOL_RUBBER, BTN_TOOL_BRUSH, BTN_TOOL_PENCIL,
                         BTN_TOOL_AIRBRUSH});
        setScanCodeState(KeyState::UP, {BTN_TOUCH, BTN_STYLUS,
                                        BTN_STYLUS2, BTN_0,
                                        BTN_TOOL_FINGER, BTN_TOOL_PEN,
                                        BTN_TOOL_RUBBER, BTN_TOOL_BRUSH,
                                        BTN_TOOL_PENCIL, BTN_TOOL_AIRBRUSH,
                                        BTN_TOOL_MOUSE, BTN_TOOL_LENS,
                                        BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
                                        BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP,
                                        BTN_LEFT, BTN_RIGHT,
                                        BTN_MIDDLE, BTN_BACK,
                                        BTN_SIDE, BTN_FORWARD,
                                        BTN_EXTRA, BTN_TASK});
        setKeyCodeState(KeyState::UP,
                        {AKEYCODE_STYLUS_BUTTON_PRIMARY, AKEYCODE_STYLUS_BUTTON_SECONDARY});
        EXPECT_CALL(mMockEventHub,
                    mapKey(EVENTHUB_ID, BTN_LEFT, 0, 0, testing::_,
                           testing::_, testing::_))
                .WillRepeatedly(Return(NAME_NOT_FOUND));
        EXPECT_CALL(mMockEventHub, hasInputProperty(EVENTHUB_ID, INPUT_PROP_BUTTONPAD))
                .WillRepeatedly(Return(true));
        EXPECT_CALL(mMockEventHub, hasInputProperty(EVENTHUB_ID, INPUT_PROP_SEMI_MT))
                .WillRepeatedly(Return(false));
        setupAxis(ABS_MT_SLOT, true, 0, 4, 0);
        setupAxis(ABS_MT_POSITION_X, true, 0, 2000, 24);
        setupAxis(ABS_MT_POSITION_Y, true, 0, 1000, 24);
        setupAxis(ABS_MT_PRESSURE, true, 0, 255, 0);
        setupAxis(ABS_MT_ORIENTATION, false, 0, 0, 0);
        setupAxis(ABS_MT_TOUCH_MAJOR, false, 0, 0, 0);
        setupAxis(ABS_MT_TOUCH_MINOR, false, 0, 0, 0);
        setupAxis(ABS_MT_WIDTH_MAJOR, false, 0, 0, 0);
        setupAxis(ABS_MT_WIDTH_MINOR, false, 0, 0, 0);
        EXPECT_CALL(mMockEventHub, getAbsoluteAxisValue(EVENTHUB_ID, ABS_MT_SLOT, testing::_))
                .WillRepeatedly([](int32_t eventHubId, int32_t, int32_t* outValue) {
                    *outValue = 0;
                    return OK;
                });
        mMapper = createInputMapper<TouchpadInputMapper>(*mDeviceContext, mReaderConfiguration);
=======
        InputMapperUnitTest::SetUp();
        expectScanCodes( true,
                        {BTN_LEFT, BTN_RIGHT, BTN_TOOL_FINGER, BTN_TOOL_QUINTTAP, BTN_TOUCH,
                         BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP});
        expectScanCodes( false,
                        {BTN_TOOL_PEN, BTN_TOOL_RUBBER, BTN_TOOL_BRUSH, BTN_TOOL_PENCIL,
                         BTN_TOOL_AIRBRUSH});
        setScanCodeState(KeyState::UP, {BTN_TOUCH, BTN_STYLUS,
                                        BTN_STYLUS2, BTN_0,
                                        BTN_TOOL_FINGER, BTN_TOOL_PEN,
                                        BTN_TOOL_RUBBER, BTN_TOOL_BRUSH,
                                        BTN_TOOL_PENCIL, BTN_TOOL_AIRBRUSH,
                                        BTN_TOOL_MOUSE, BTN_TOOL_LENS,
                                        BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
                                        BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP,
                                        BTN_LEFT, BTN_RIGHT,
                                        BTN_MIDDLE, BTN_BACK,
                                        BTN_SIDE, BTN_FORWARD,
                                        BTN_EXTRA, BTN_TASK});
        setKeyCodeState(KeyState::UP,
                        {AKEYCODE_STYLUS_BUTTON_PRIMARY, AKEYCODE_STYLUS_BUTTON_SECONDARY});
        EXPECT_CALL(mMockEventHub,
                    mapKey(EVENTHUB_ID, BTN_LEFT, 0, 0, testing::_,
                           testing::_, testing::_))
                .WillRepeatedly(Return(NAME_NOT_FOUND));
        EXPECT_CALL(mMockEventHub, hasInputProperty(EVENTHUB_ID, INPUT_PROP_BUTTONPAD))
                .WillRepeatedly(Return(true));
        EXPECT_CALL(mMockEventHub, hasInputProperty(EVENTHUB_ID, INPUT_PROP_SEMI_MT))
                .WillRepeatedly(Return(false));
        setupAxis(ABS_MT_SLOT, true, 0, 4, 0);
        setupAxis(ABS_MT_POSITION_X, true, 0, 2000, 24);
        setupAxis(ABS_MT_POSITION_Y, true, 0, 1000, 24);
        setupAxis(ABS_MT_PRESSURE, true, 0, 255, 0);
        setupAxis(ABS_MT_ORIENTATION, false, 0, 0, 0);
        setupAxis(ABS_MT_TOUCH_MAJOR, false, 0, 0, 0);
        setupAxis(ABS_MT_TOUCH_MINOR, false, 0, 0, 0);
        setupAxis(ABS_MT_WIDTH_MAJOR, false, 0, 0, 0);
        setupAxis(ABS_MT_WIDTH_MINOR, false, 0, 0, 0);
        setupAxis(ABS_MT_TRACKING_ID, false, 0, 0, 0);
        setupAxis(ABS_MT_DISTANCE, false, 0, 0, 0);
        setupAxis(ABS_MT_TOOL_TYPE, false, 0, 0, 0);
        EXPECT_CALL(mMockEventHub, getAbsoluteAxisValue(EVENTHUB_ID, ABS_MT_SLOT, testing::_))
                .WillRepeatedly([](int32_t eventHubId, int32_t, int32_t* outValue) {
                    *outValue = 0;
                    return OK;
                });
        EXPECT_CALL(mMockEventHub, getMtSlotValues(EVENTHUB_ID, testing::_, testing::_))
                .WillRepeatedly([]() -> base::Result<std::vector<int32_t>> {
                    return base::ResultError("Axis not supported", NAME_NOT_FOUND);
                });
        mMapper = createInputMapper<TouchpadInputMapper>(*mDeviceContext, mReaderConfiguration);
>>>>>>> cb7de99672a8aee59438bc783bdaceec9ecf9d4e
    }
};
TEST_F(MultiTouchInputMapperUnitTest, MultiFingerGestureWithUnexpectedReset,
       TouchpadInputMapperTest, HoverAndLeftButtonPress) {
    std::list<NotifyArgs> args;
    constexpr int32_t FIRST_TRACKING_ID = 1, SECOND_TRACKING_ID = 2;
    int32_t x1 = 100, y1 = 125, x2 = 200, y2 = 225;
    processKey(BTN_TOUCH, 1);
    args += processPosition(x1, y1);
    args += processId(FIRST_TRACKING_ID);
    args += processSlot(1);
    args += processPosition(x2, y2);
    args += processId(SECOND_TRACKING_ID);
    ASSERT_THAT(args, IsEmpty());
    args = processSync();
    ASSERT_THAT(args,
                ElementsAre(VariantWith<NotifyMotionArgs>(
                                    WithMotionAction(AMOTION_EVENT_ACTION_DOWN)),
                            VariantWith<NotifyMotionArgs>(
                                    WithMotionAction(ACTION_POINTER_1_DOWN))));
    x1 += 10;
    y1 += 15;
    x2 += 5;
    y2 -= 10;
    args = processSlot(0);
    args += processPosition(x1, y1);
    args += processSlot(1);
    args += processPosition(x2, y2);
    ASSERT_THAT(args, IsEmpty());
    args = processSync();
    ASSERT_THAT(args,
                ElementsAre(VariantWith<NotifyMotionArgs>(
                        WithMotionAction(AMOTION_EVENT_ACTION_MOVE))));
    const auto pointerCoordsBeforeReset = std::get<NotifyMotionArgs>(args.back()).pointerCoords;
    EXPECT_CALL(mMockEventHub, getAbsoluteAxisValue(EVENTHUB_ID, ABS_MT_SLOT, _))
            .WillRepeatedly([=](int32_t, int32_t, int32_t* outValue) {
                *outValue = 1;
                return OK;
            });
    mockSlotValues(
            {{1, {Point{x1, y1}, FIRST_TRACKING_ID}}, {2, {Point{x2, y2}, SECOND_TRACKING_ID}}});
    setScanCodeState(KeyState::DOWN, {BTN_TOUCH});
    args = mMapper->reset(systemTime(SYSTEM_TIME_MONOTONIC));
    ASSERT_THAT(args,
                ElementsAre(VariantWith<NotifyMotionArgs>(
                        WithMotionAction(AMOTION_EVENT_ACTION_CANCEL))));
    args = processSync();
    ASSERT_THAT(args,
                ElementsAre(VariantWith<NotifyMotionArgs>(
                                    WithMotionAction(AMOTION_EVENT_ACTION_DOWN)),
                            VariantWith<NotifyMotionArgs>(
                                    WithMotionAction(ACTION_POINTER_1_DOWN))));
    ASSERT_EQ(std::get<NotifyMotionArgs>(args.back()).pointerCoords, pointerCoordsBeforeReset);
    x1 += 10;
    y1 += 15;
    x2 += 5;
    y2 -= 10;
    args = processSlot(0);
    args += processPosition(x1, y1);
    args += processSlot(1);
    args += processPosition(x2, y2);
    ASSERT_THAT(args, IsEmpty());
    args = processSync();
    ASSERT_THAT(args,
                ElementsAre(VariantWith<NotifyMotionArgs>(
                        WithMotionAction(AMOTION_EVENT_ACTION_MOVE))));
    args = processSlot(0);
    args += processId(-1);
    ASSERT_THAT(args, IsEmpty());
    args = processSync();
    ASSERT_THAT(args,
                ElementsAre(VariantWith<NotifyMotionArgs>(WithMotionAction(ACTION_POINTER_0_UP))));
    processKey(BTN_TOUCH, 0);
    args = processSlot(1);
    args += processId(-1);
    ASSERT_THAT(args, IsEmpty());
    args = processSync();
    ASSERT_THAT(args,
                ElementsAre(
                        VariantWith<NotifyMotionArgs>(WithMotionAction(AMOTION_EVENT_ACTION_UP))));
}
class TouchpadInputMapperTestWithChoreographer : public TouchpadInputMapperTestBase {
protected:
    void SetUp() override {
        input_flags::enable_pointer_choreographer(true);
        TouchpadInputMapperTestBase::SetUp();
    }
};
}
