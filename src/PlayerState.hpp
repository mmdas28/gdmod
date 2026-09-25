#pragma once

#include <Geode/Geode.hpp>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace rp {

#define RP_PLAYER_FIELDS(X)            \
    X(m_wasTeleported)                 \
    X(m_fixGravityBug)                 \
    X(m_reverseSync)                   \
    X(m_yVelocityBeforeSlope)          \
    X(m_dashX)                         \
    X(m_dashY)                         \
    X(m_dashAngle)                     \
    X(m_dashStartTime)                 \
    X(m_dashRing)                      \
    X(m_slopeStartTime)                \
    X(m_maybeLastGroundObject)         \
    X(m_lastCollisionBottom)           \
    X(m_lastCollisionTop)              \
    X(m_lastCollisionLeft)             \
    X(m_lastCollisionRight)            \
    X(m_unk50C)                        \
    X(m_unk510)                        \
    X(m_currentSlope2)                 \
    X(m_preLastGroundObject)           \
    X(m_slopeAngle)                    \
    X(m_slopeSlidingMaybeRotated)      \
    X(m_quickCheckpointMode)           \
    X(m_collidedObject)                \
    X(m_lastGroundObject)              \
    X(m_collidingWithLeft)             \
    X(m_collidingWithRight)            \
    X(m_maybeSavedPlayerFrame)         \
    X(m_scaleXRelated2)                \
    X(m_groundYVelocity)               \
    X(m_yVelocityRelated)              \
    X(m_scaleXRelated3)                \
    X(m_scaleXRelated4)                \
    X(m_scaleXRelated5)                \
    X(m_isCollidingWithSlope)          \
    X(m_isBallRotating)                \
    X(m_unk669)                        \
    X(m_currentPotentialSlope)         \
    X(m_currentSlope)                  \
    X(unk_584)                         \
    X(m_collidingWithSlopeId)          \
    X(m_slopeFlipGravityRelated)       \
    X(m_slopeAngleRadians)             \
    X(m_rotateObjectsRelated)          \
    X(m_potentialSlopeMap)             \
    X(m_rotationSpeed)                 \
    X(m_rotateSpeed)                   \
    X(m_isRotating)                    \
    X(m_isBallRotating2)               \
    X(m_speedMultiplier)               \
    X(m_yStart)                        \
    X(m_gravity)                       \
    X(m_gameModeChangedTime)           \
    X(m_padRingRelated)                \
    X(m_maybeIsFalling)                \
    X(m_maybeCanRunIntoBlocks)         \
    X(m_isOnGround3)                   \
    X(m_lastJumpTime)                  \
    X(m_lastFlipTime)                  \
    X(m_lastSpiderFlipTime)            \
    X(m_accelerationOrSpeed)           \
    X(m_snapDistance)                  \
    X(m_ringJumpRelated)               \
    X(m_ringRelatedSet)                \
    X(m_objectSnappedTo)               \
    X(m_slopeRotation)                 \
    X(m_currentSlopeYVelocity)         \
    X(m_unk3d0)                        \
    X(m_blackOrbRelated)               \
    X(m_unk3e0)                        \
    X(m_unk3e1)                        \
    X(m_isAccelerating)                \
    X(m_isCurrentSlopeTop)             \
    X(m_collidedTopMinY)               \
    X(m_collidedBottomMaxY)            \
    X(m_collidedLeftMaxX)              \
    X(m_collidedRightMinX)             \
    X(m_maybeIsColliding)              \
    X(m_jumpBuffered)                  \
    X(m_stateRingJump)                 \
    X(m_wasJumpBuffered)               \
    X(m_wasRobotJump)                  \
    X(m_stateJumpBuffered)             \
    X(m_stateRingJump2)                \
    X(m_touchedRing)                   \
    X(m_touchedCustomRing)             \
    X(m_touchedGravityPortal)          \
    X(m_maybeTouchedBreakableBlock)    \
    X(m_jumpRelatedAC2)                \
    X(m_touchedPad)                    \
    X(m_yVelocity)                     \
    X(m_fallSpeed)                     \
    X(m_isOnSlope)                     \
    X(m_wasOnSlope)                    \
    X(m_slopeVelocity)                 \
    X(m_maybeUpsideDownSlope)          \
    X(m_isOnGround)                    \
    X(m_reverseRelated)                \
    X(m_maybeReverseSpeed)             \
    X(m_maybeReverseAcceleration)      \
    X(m_xVelocityRelated2)             \
    X(m_groundObjectMaterial)          \
    X(m_shipRotation)                  \
    X(m_lastPortalPos)                 \
    X(m_isOnGround2)                   \
    X(m_lastLandTime)                  \
    X(m_platformerVelocityRelated)     \
    X(m_maybeIsBoosted)                \
    X(m_scaleXRelatedTime)             \
    X(m_decreaseBoostSlide)            \
    X(m_unkA29)                        \
    X(m_isLocked)                      \
    X(m_controlsDisabled)              \
    X(m_lastGroundedPos)               \
    X(m_touchedRings)                  \
    X(m_lastActivatedPortal)           \
    X(m_hasEverJumped)                 \
    X(m_hasEverHitRing)                \
    X(m_position)                      \
    X(m_totalTime)                     \
    X(m_isBeingSpawnedByDualPortal)    \
    X(m_unkAngle1)                     \
    X(m_yVelocityRelated3)             \
    X(m_followRelated)                 \
    X(m_playerFollowFloats)            \
    X(m_unk838)                        \
    X(m_stateOnGround)                 \
    X(m_stateUnk)                      \
    X(m_stateNoStickX)                 \
    X(m_stateNoStickY)                 \
    X(m_stateUnk2)                     \
    X(m_stateBoostX)                   \
    X(m_stateBoostY)                   \
    X(m_maybeStateForce2)              \
    X(m_stateScale)                    \
    X(m_platformerXVelocity)           \
    X(m_holdingRight)                  \
    X(m_holdingLeft)                   \
    X(m_leftPressedFirst)              \
    X(m_scaleXRelated)                 \
    X(m_maybeHasStopped)               \
    X(m_xVelocityRelated)              \
    X(m_maybeGoingCorrectSlopeDirection) \
    X(m_isSliding)                     \
    X(m_maybeSlopeForce)               \
    X(m_isOnIce)                       \
    X(m_physDeltaRelated)              \
    X(m_isOnGround4)                   \
    X(m_maybeSlidingTime)              \
    X(m_maybeSlidingStartTime)         \
    X(m_changedDirectionsTime)         \
    X(m_slopeEndTime)                  \
    X(m_isMoving)                      \
    X(m_platformerMovingLeft)          \
    X(m_platformerMovingRight)         \
    X(m_isSlidingRight)                \
    X(m_maybeChangedDirectionAngle)    \
    X(m_stateNoAutoJump)               \
    X(m_stateDartSlide)                \
    X(m_stateHitHead)                  \
    X(m_stateFlipGravity)              \
    X(m_gravityMod)                    \
    X(m_stateForce)                    \
    X(m_stateForceVector)              \
    X(m_affectedByForces)              \
    X(m_jumpPadRelated)                \
    X(m_lastMovedTime)                 \
    X(m_playerSpeedAC)                 \
    X(m_fixRobotJump)                  \
    X(m_holdingButtons)                \
    X(m_inputsLocked)                  \
    X(m_isOutOfBounds)                 \
    X(m_fallStartY)

struct PlayerSnapshot {
#define RP_DECLARE_FIELD(name) std::remove_cvref_t<decltype(std::declval<PlayerObject&>().name)> name{};
    RP_PLAYER_FIELDS(RP_DECLARE_FIELD)
#undef RP_DECLARE_FIELD

    cocos2d::CCPoint nodePosition;
    float nodeRotation = 0.f;
    double positionX = 0.0;
    double positionY = 0.0;
    cocos2d::CCPoint lastPosition;
    bool valid = false;

    void capture(PlayerObject* player) {
        if (!player) {
            valid = false;
            return;
        }
#define RP_CAPTURE_FIELD(name) this->name = player->name;
        RP_PLAYER_FIELDS(RP_CAPTURE_FIELD)
#undef RP_CAPTURE_FIELD
        nodePosition = player->getPosition();
        nodeRotation = player->getRotation();
        positionX = player->m_positionX;
        positionY = player->m_positionY;
        lastPosition = player->m_lastPosition;
        valid = true;
    }

    void apply(PlayerObject* player) const {
        if (!player || !valid) return;
        player->setPosition(nodePosition);
        player->setRotation(nodeRotation);
#define RP_APPLY_FIELD(name) player->name = this->name;
        RP_PLAYER_FIELDS(RP_APPLY_FIELD)
#undef RP_APPLY_FIELD
        player->m_positionX = positionX;
        player->m_positionY = positionY;
        player->m_lastPosition = lastPosition;
    }
};

inline uint64_t mixHash(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h;
}

inline uint64_t quantize(double v, double scale) {
    return static_cast<uint64_t>(static_cast<int64_t>(std::llround(v * scale)));
}

inline uint64_t hashPlayer(PlayerObject* p) {
    if (!p) return 0;
    uint64_t h = 1469598103934665603ull;
    auto nodePos = p->getPosition();
    h = mixHash(h, quantize(nodePos.x, 1000.0));
    h = mixHash(h, quantize(nodePos.y, 1000.0));
    h = mixHash(h, quantize(p->m_position.x, 1000.0));
    h = mixHash(h, quantize(p->m_position.y, 1000.0));
    h = mixHash(h, quantize(p->m_positionX, 1000.0));
    h = mixHash(h, quantize(p->m_positionY, 1000.0));
    h = mixHash(h, quantize(p->m_yVelocity, 1000.0));
    h = mixHash(h, quantize(p->m_vehicleSize, 1000.0));
    h = mixHash(h, quantize(p->m_playerSpeed, 1000.0));
    h = mixHash(h, quantize(p->m_gravityMod, 1000.0));
    uint64_t flags = 0;
    int bit = 0;
    auto push = [&](bool b) { flags |= (static_cast<uint64_t>(b) << bit++); };
    push(p->m_isShip);
    push(p->m_isBird);
    push(p->m_isBall);
    push(p->m_isDart);
    push(p->m_isRobot);
    push(p->m_isSpider);
    push(p->m_isSwing);
    push(p->m_isUpsideDown);
    push(p->m_isOnGround);
    push(p->m_isDashing);
    push(p->m_isSideways);
    push(p->m_isGoingLeft);
    push(p->m_jumpBuffered);
    push(p->m_isOnSlope);
    push(p->m_stateRingJump);
    push(p->m_touchedRing);
    push(p->m_touchedPad);
    push(p->m_hasEverJumped);
    h = mixHash(h, flags);
    h = mixHash(h, static_cast<uint64_t>(p->m_stateJumpBuffered));
    h = mixHash(h, reinterpret_cast<uintptr_t>(p->m_lastActivatedPortal));
    h = mixHash(h, static_cast<uint64_t>(p->m_touchedRings.size()));
    h = mixHash(h, static_cast<uint64_t>(p->m_ringRelatedSet.size()));
    h = mixHash(h, static_cast<uint64_t>(p->m_jumpPadRelated.size()));
    return h;
}

}
