/*
 * Copyright 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
constexpr std::optional<uint8_t> NO_PORT = std::nullopt; /**
                                                          * Unit tests for TouchpadInputMapper.
                                                          */
class TouchpadInputMapperTestBase : public InputMapperUnitTest {
protected:
    void SetUp() override {
        InputMapperUnitTest::SetUp();

        // Present scan codes: BTN_TOUCH and BTN_TOOL_FINGER
        expectScanCodes(/*present=*/true,
                        {BTN_LEFT, BTN_RIGHT, BTN_TOOL_FINGER, BTN_TOOL_QUINTTAP, BTN_TOUCH,
                         BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP});
        // Missing scan codes that the mapper checks for.
        expectScanCodes(/*present=*/false,
                        {BTN_TOOL_PEN, BTN_TOOL_RUBBER, BTN_TOOL_BRUSH, BTN_TOOL_PENCIL,
                         BTN_TOOL_AIRBRUSH});

        // Current scan code state - all keys are UP by default
        setScanCodeState(KeyState::UP, {BTN_TOUCH,          BTN_STYLUS,
                                        BTN_STYLUS2,        BTN_0,
                                        BTN_TOOL_FINGER,    BTN_TOOL_PEN,
                                        BTN_TOOL_RUBBER,    BTN_TOOL_BRUSH,
                                        BTN_TOOL_PENCIL,    BTN_TOOL_AIRBRUSH,
                                        BTN_TOOL_MOUSE,     BTN_TOOL_LENS,
                                        BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
                                        BTN_TOOL_QUADTAP,   BTN_TOOL_QUINTTAP,
                                        BTN_LEFT,           BTN_RIGHT,
                                        BTN_MIDDLE,         BTN_BACK,
                                        BTN_SIDE,           BTN_FORWARD,
                                        BTN_EXTRA,          BTN_TASK});

        setKeyCodeState(KeyState::UP,
                        {AKEYCODE_STYLUS_BUTTON_PRIMARY, AKEYCODE_STYLUS_BUTTON_SECONDARY});

        // Key mappings
        EXPECT_CALL(mMockEventHub,
                    mapKey(EVENTHUB_ID, BTN_LEFT, /*usageCode=*/0, /*metaState=*/0, testing::_,
                           testing::_, testing::_))
                .WillRepeatedly(Return(NAME_NOT_FOUND));

        // Input properties - only INPUT_PROP_BUTTONPAD
        EXPECT_CALL(mMockEventHub, hasInputProperty(EVENTHUB_ID, INPUT_PROP_BUTTONPAD))
                .WillRepeatedly(Return(true));
        EXPECT_CALL(mMockEventHub, hasInputProperty(EVENTHUB_ID, INPUT_PROP_SEMI_MT))
                .WillRepeatedly(Return(false));

        // Axes that the device has
        setupAxis(ABS_MT_SLOT, /*valid=*/true, /*min=*/0, /*max=*/4, /*resolution=*/0);
        setupAxis(ABS_MT_POSITION_X, /*valid=*/true, /*min=*/0, /*max=*/2000, /*resolution=*/24);
        setupAxis(ABS_MT_POSITION_Y, /*valid=*/true, /*min=*/0, /*max=*/1000, /*resolution=*/24);
        setupAxis(ABS_MT_PRESSURE, /*valid=*/true, /*min*/ 0, /*max=*/255, /*resolution=*/0);
        // Axes that the device does not have
        setupAxis(ABS_MT_ORIENTATION, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_TOUCH_MAJOR, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_TOUCH_MINOR, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_WIDTH_MAJOR, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_WIDTH_MINOR, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_TRACKING_ID, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_DISTANCE, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_TOOL_TYPE, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);

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

        // Present scan codes: BTN_TOUCH and BTN_TOOL_FINGER
        expectScanCodes(/*present=*/true,
                        {BTN_LEFT, BTN_RIGHT, BTN_TOOL_FINGER, BTN_TOOL_QUINTTAP, BTN_TOUCH,
                         BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP});
        // Missing scan codes that the mapper checks for.
        expectScanCodes(/*present=*/false,
                        {BTN_TOOL_PEN, BTN_TOOL_RUBBER, BTN_TOOL_BRUSH, BTN_TOOL_PENCIL,
                         BTN_TOOL_AIRBRUSH});

        // Current scan code state - all keys are UP by default
        setScanCodeState(KeyState::UP, {BTN_TOUCH,          BTN_STYLUS,
                                        BTN_STYLUS2,        BTN_0,
                                        BTN_TOOL_FINGER,    BTN_TOOL_PEN,
                                        BTN_TOOL_RUBBER,    BTN_TOOL_BRUSH,
                                        BTN_TOOL_PENCIL,    BTN_TOOL_AIRBRUSH,
                                        BTN_TOOL_MOUSE,     BTN_TOOL_LENS,
                                        BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
                                        BTN_TOOL_QUADTAP,   BTN_TOOL_QUINTTAP,
                                        BTN_LEFT,           BTN_RIGHT,
                                        BTN_MIDDLE,         BTN_BACK,
                                        BTN_SIDE,           BTN_FORWARD,
                                        BTN_EXTRA,          BTN_TASK});

        setKeyCodeState(KeyState::UP,
                        {AKEYCODE_STYLUS_BUTTON_PRIMARY, AKEYCODE_STYLUS_BUTTON_SECONDARY});

        // Key mappings
        EXPECT_CALL(mMockEventHub,
                    mapKey(EVENTHUB_ID, BTN_LEFT, /*usageCode=*/0, /*metaState=*/0, testing::_,
                           testing::_, testing::_))
                .WillRepeatedly(Return(NAME_NOT_FOUND));

        // Input properties - only INPUT_PROP_BUTTONPAD
        EXPECT_CALL(mMockEventHub, hasInputProperty(EVENTHUB_ID, INPUT_PROP_BUTTONPAD))
                .WillRepeatedly(Return(true));
        EXPECT_CALL(mMockEventHub, hasInputProperty(EVENTHUB_ID, INPUT_PROP_SEMI_MT))
                .WillRepeatedly(Return(false));

        // Axes that the device has
        setupAxis(ABS_MT_SLOT, /*valid=*/true, /*min=*/0, /*max=*/4, /*resolution=*/0);
        setupAxis(ABS_MT_POSITION_X, /*valid=*/true, /*min=*/0, /*max=*/2000, /*resolution=*/24);
        setupAxis(ABS_MT_POSITION_Y, /*valid=*/true, /*min=*/0, /*max=*/1000, /*resolution=*/24);
        setupAxis(ABS_MT_PRESSURE, /*valid=*/true, /*min*/ 0, /*max=*/255, /*resolution=*/0);
        // Axes that the device does not have
        setupAxis(ABS_MT_ORIENTATION, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_TOUCH_MAJOR, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_TOUCH_MINOR, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_WIDTH_MAJOR, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_WIDTH_MINOR, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);

        EXPECT_CALL(mMockEventHub, getAbsoluteAxisValue(EVENTHUB_ID, ABS_MT_SLOT, testing::_))
                .WillRepeatedly([](int32_t eventHubId, int32_t, int32_t* outValue) {
                    *outValue = 0;
                    return OK;
                });
        mMapper = createInputMapper<TouchpadInputMapper>(*mDeviceContext, mReaderConfiguration);
=======
        InputMapperUnitTest::SetUp();

        // Present scan codes: BTN_TOUCH and BTN_TOOL_FINGER
        expectScanCodes(/*present=*/true,
                        {BTN_LEFT, BTN_RIGHT, BTN_TOOL_FINGER, BTN_TOOL_QUINTTAP, BTN_TOUCH,
                         BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP});
        // Missing scan codes that the mapper checks for.
        expectScanCodes(/*present=*/false,
                        {BTN_TOOL_PEN, BTN_TOOL_RUBBER, BTN_TOOL_BRUSH, BTN_TOOL_PENCIL,
                         BTN_TOOL_AIRBRUSH});

        // Current scan code state - all keys are UP by default
        setScanCodeState(KeyState::UP, {BTN_TOUCH,          BTN_STYLUS,
                                        BTN_STYLUS2,        BTN_0,
                                        BTN_TOOL_FINGER,    BTN_TOOL_PEN,
                                        BTN_TOOL_RUBBER,    BTN_TOOL_BRUSH,
                                        BTN_TOOL_PENCIL,    BTN_TOOL_AIRBRUSH,
                                        BTN_TOOL_MOUSE,     BTN_TOOL_LENS,
                                        BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
                                        BTN_TOOL_QUADTAP,   BTN_TOOL_QUINTTAP,
                                        BTN_LEFT,           BTN_RIGHT,
                                        BTN_MIDDLE,         BTN_BACK,
                                        BTN_SIDE,           BTN_FORWARD,
                                        BTN_EXTRA,          BTN_TASK});

        setKeyCodeState(KeyState::UP,
                        {AKEYCODE_STYLUS_BUTTON_PRIMARY, AKEYCODE_STYLUS_BUTTON_SECONDARY});

        // Key mappings
        EXPECT_CALL(mMockEventHub,
                    mapKey(EVENTHUB_ID, BTN_LEFT, /*usageCode=*/0, /*metaState=*/0, testing::_,
                           testing::_, testing::_))
                .WillRepeatedly(Return(NAME_NOT_FOUND));

        // Input properties - only INPUT_PROP_BUTTONPAD
        EXPECT_CALL(mMockEventHub, hasInputProperty(EVENTHUB_ID, INPUT_PROP_BUTTONPAD))
                .WillRepeatedly(Return(true));
        EXPECT_CALL(mMockEventHub, hasInputProperty(EVENTHUB_ID, INPUT_PROP_SEMI_MT))
                .WillRepeatedly(Return(false));

        // Axes that the device has
        setupAxis(ABS_MT_SLOT, /*valid=*/true, /*min=*/0, /*max=*/4, /*resolution=*/0);
        setupAxis(ABS_MT_POSITION_X, /*valid=*/true, /*min=*/0, /*max=*/2000, /*resolution=*/24);
        setupAxis(ABS_MT_POSITION_Y, /*valid=*/true, /*min=*/0, /*max=*/1000, /*resolution=*/24);
        setupAxis(ABS_MT_PRESSURE, /*valid=*/true, /*min*/ 0, /*max=*/255, /*resolution=*/0);
        // Axes that the device does not have
        setupAxis(ABS_MT_ORIENTATION, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_TOUCH_MAJOR, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_TOUCH_MINOR, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_WIDTH_MAJOR, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_WIDTH_MINOR, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_TRACKING_ID, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_DISTANCE, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);
        setupAxis(ABS_MT_TOOL_TYPE, /*valid=*/false, /*min=*/0, /*max=*/0, /*resolution=*/0);

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

// This test simulates a multi-finger gesture with unexpected reset in between. This might happen
// due to buffer overflow and device with report a SYN_DROPPED. In this case we expect mapper to be
// reset, MT slot state to be re-populated and the gesture should be cancelled and restarted.
TEST_F(MultiTouchInputMapperUnitTest, MultiFingerGestureWithUnexpectedReset,
       TouchpadInputMapperTest, HoverAndLeftButtonPress) {
    std::list<NotifyArgs> args;

    // Two fingers down at once.
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

    // Move.
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

    // On buffer overflow mapper will be reset and MT slots data will be repopulated
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

    // SYN_REPORT should restart the gesture again
    args = processSync();
    ASSERT_THAT(args,
                ElementsAre(VariantWith<NotifyMotionArgs>(
                                    WithMotionAction(AMOTION_EVENT_ACTION_DOWN)),
                            VariantWith<NotifyMotionArgs>(
                                    WithMotionAction(ACTION_POINTER_1_DOWN))));
    ASSERT_EQ(std::get<NotifyMotionArgs>(args.back()).pointerCoords, pointerCoordsBeforeReset);

    // Move.
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

    // First finger up.
    args = processSlot(0);
    args += processId(-1);
    ASSERT_THAT(args, IsEmpty());

    args = processSync();
    ASSERT_THAT(args,
                ElementsAre(VariantWith<NotifyMotionArgs>(WithMotionAction(ACTION_POINTER_0_UP))));

    // Second finger up.
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

} // namespace android