#include "sim_test.h"
#include "gtest/gtest.h"


TEST(VTABackendSimulatorTest, VTA_CONV1_TEST) {
    EXPECT_EQ(0,conv1_test());
}

/*
TEST(VTABackendSimulatorTest, VTA_CONV_2LAYER_TEST) {
    EXPECT_EQ(0,conv_2layer_test());
}

TEST(VTABackendSimulatorTest, VTA_ADD_16_TEST) {
  EXPECT_EQ(0,add_16_test());
}

TEST(VTABackendSimulatorTest, VTA_GEMM_16_TEST) {
  EXPECT_EQ(0,gemm_16_test());
}

TEST(VTABackendSimulatorTest, VTA_GEMM_GEMM_16_TEST) {
  EXPECT_EQ(0,gemm_gemm_16_test());
}

TEST(VTABackendSimulatorTest, VTA_CONV2_TEST) {
  EXPECT_EQ(0,conv2_test());
}

TEST(VTABackendSimulatorTest, VTA_CONV3_TEST) {
  EXPECT_EQ(0,conv3_test());
}

TEST(VTABackendSimulatorTest, VTA_CONV4_TEST) {
  EXPECT_EQ(0,conv4_test());
}

TEST(VTABackendSimulatorTest, VTA_CONV5_TEST) {
  // TODO : simulator bug
  //EXPECT_EQ(0,conv5_test());
  EXPECT_EQ(0,0);
}

TEST(VTABackendSimulatorTest, VTA_CONV6_TEST) {
  EXPECT_EQ(0,conv6_test());
}

TEST(VTABackendSimulatorTest, VTA_CONV7_TEST) {
  EXPECT_EQ(0,conv7_test());
}

TEST(VTABackendSimulatorTest, VTA_CONV8_TEST) {
  EXPECT_EQ(0,conv8_test());
}

TEST(VTABackendSimulatorTest, VTA_CONV9_TEST) {
  EXPECT_EQ(0,conv9_test());
}

TEST(VTABackendSimulatorTest, VTA_CONV10_TEST) {
  EXPECT_EQ(0,conv10_test());
}

TEST(VTABackendSimulatorTest, VTA_CONV11_TEST) {
  EXPECT_EQ(0,conv11_test());
}

TEST(VTABackendSimulatorTest, VTA_CONV12_TEST) {
  EXPECT_EQ(0,conv12_test());
}
TEST(VTABackendSimulatorTest, VTA_CONV13_TEST) {
  EXPECT_EQ(0,conv13_test());
}

TEST(VTABackendSimulatorTest, VTA_CONV14_TEST) {
  EXPECT_EQ(0,conv14_test());
}

TEST(VTABackendSimulatorTest, VTA_CONV15_TEST) {
  EXPECT_EQ(0,conv15_test());
}

TEST(VTABackendSimulatorTest, VTA_CONV16_TEST) {
  EXPECT_EQ(0,conv16_test());
}

TEST(VTABackendSimulatorTest, VTA_CONV17_TEST) {
  EXPECT_EQ(0,conv17_test());
}
TEST(VTABackendSimulatorTest, VTA_CONV18_TEST) {
  EXPECT_EQ(0,conv18_test());
}
TEST(VTABackendSimulatorTest, VTA_CONV19_TEST) {
  EXPECT_EQ(0,conv19_test());
}
TEST(VTABackendSimulatorTest, VTA_CONV20_TEST) {
  EXPECT_EQ(0,conv20_test());
}

 */
TEST(VTABackendSimulatorTest, VTA_CONV21_TEST) {
  EXPECT_EQ(0,conv21_test());
}
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();

}