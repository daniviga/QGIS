/***************************************************************************
  qgsdemterraintilegeometry_p.cpp
  --------------------------------------
  Date                 : July 2017
  Copyright            : (C) 2017 by Martin Dobias
  Email                : wonder dot sk at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include <QMatrix4x4>
#include "qgsdemterraintilegeometry_p.h"
#include <Qt3DRender/qattribute.h>
#include <Qt3DRender/qbuffer.h>
#include <Qt3DRender/qbufferdatagenerator.h>
#include <limits>
#include <cmath>
#include "qgsraycastingutils_p.h"

///@cond PRIVATE

using namespace Qt3DRender;


static QByteArray createPlaneVertexData( int res, float skirtHeight, const QByteArray &heights )
{
  Q_ASSERT( res >= 2 );
  Q_ASSERT( heights.count() == res * res * ( int )sizeof( float ) );

  const float *zData = ( const float * ) heights.constData();
  const float *zBits = zData;

  const int nVerts = ( res + 2 ) * ( res + 2 );

  // Populate a buffer with the interleaved per-vertex data with
  // vec3 pos, vec2 texCoord, vec3 normal, vec4 tangent
  const quint32 elementSize = 3 + 2 + 3;
  const quint32 stride = elementSize * sizeof( float );
  QByteArray bufferBytes;
  bufferBytes.resize( stride * nVerts );
  float *fptr = reinterpret_cast<float *>( bufferBytes.data() );

  float w = 1, h = 1;
  QSize resolution( res, res );
  const float x0 = -w / 2.0f;
  const float z0 = -h / 2.0f;
  const float dx = w / ( resolution.width() - 1 );
  const float dz = h / ( resolution.height() - 1 );
  const float du = 1.0 / ( resolution.width() - 1 );
  const float dv = 1.0 / ( resolution.height() - 1 );

  // the height of vertices with no-data value... the value should not really matter
  // as we do not create valid triangles that would use such vertices
  const float noDataHeight = 0;

  // Iterate over z
  for ( int j = -1; j <= resolution.height(); ++j )
  {
    int jBound = qBound( 0, j, resolution.height() - 1 );
    const float z = z0 + static_cast<float>( jBound ) * dz;
    const float v = static_cast<float>( jBound ) * dv;

    // Iterate over x
    for ( int i = -1; i <= resolution.width(); ++i )
    {
      int iBound = qBound( 0, i, resolution.width() - 1 );
      const float x = x0 + static_cast<float>( iBound ) * dx;
      const float u = static_cast<float>( iBound ) * du;

      float height;
      if ( i == iBound && j == jBound )
        height = *zBits++;
      else
        height = zData[ jBound * resolution.width() + iBound ] - skirtHeight;

      if ( std::isnan( height ) )
        height = noDataHeight;

      // position
      *fptr++ = x;
      *fptr++ = height;
      *fptr++ = z;

      // texture coordinates
      *fptr++ = u;
      *fptr++ = v;

      // TODO: compute correct normals based on neighboring pixels
      // normal
      *fptr++ = 0.0f;
      *fptr++ = 1.0f;
      *fptr++ = 0.0f;
    }
  }

  return bufferBytes;
}

inline int ijToHeightMapIndex( int i, int j, int numVerticesX, int numVerticesZ )
{
  i = qBound( 1, i, numVerticesX - 1 ) - 1;
  j = qBound( 1, j, numVerticesZ - 1 ) - 1;
  return j * ( numVerticesX - 2 ) + i;
}


static bool hasNoData( int i, int j, const float *heightMap, int numVerticesX, int numVerticesZ )
{
  return std::isnan( heightMap[ ijToHeightMapIndex( i, j, numVerticesX, numVerticesZ ) ] ) ||
         std::isnan( heightMap[ ijToHeightMapIndex( i + 1, j, numVerticesX, numVerticesZ ) ] ) ||
         std::isnan( heightMap[ ijToHeightMapIndex( i, j + 1, numVerticesX, numVerticesZ ) ] ) ||
         std::isnan( heightMap[ ijToHeightMapIndex( i + 1, j + 1, numVerticesX, numVerticesZ ) ] );
}

static QByteArray createPlaneIndexData( int res, const QByteArray &heightMap )
{
  QSize resolution( res, res );
  int numVerticesX = resolution.width() + 2;
  int numVerticesZ = resolution.height() + 2;

  // Create the index data. 2 triangles per rectangular face
  const int faces = 2 * ( numVerticesX - 1 ) * ( numVerticesZ - 1 );
  const quint32 indices = 3 * faces;
  Q_ASSERT( indices < std::numeric_limits<quint32>::max() );
  QByteArray indexBytes;
  indexBytes.resize( indices * sizeof( quint32 ) );
  quint32 *indexPtr = reinterpret_cast<quint32 *>( indexBytes.data() );

  const float *heightMapFloat = reinterpret_cast<const float *>( heightMap.constData() );

  // Iterate over z
  for ( int j = 0; j < numVerticesZ - 1; ++j )
  {
    const int rowStartIndex = j * numVerticesX;
    const int nextRowStartIndex = ( j + 1 ) * numVerticesX;

    // Iterate over x
    for ( int i = 0; i < numVerticesX - 1; ++i )
    {
      if ( hasNoData( i, j, heightMapFloat, numVerticesX, numVerticesZ ) )
      {
        // at least one corner of the quad has no-data value
        // so let's make two invalid triangles
        *indexPtr++ = rowStartIndex + i;
        *indexPtr++ = rowStartIndex + i;
        *indexPtr++ = rowStartIndex + i;

        *indexPtr++ = rowStartIndex + i;
        *indexPtr++ = rowStartIndex + i;
        *indexPtr++ = rowStartIndex + i;
        continue;
      }

      // Split quad into two triangles
      *indexPtr++ = rowStartIndex + i;
      *indexPtr++ = nextRowStartIndex + i;
      *indexPtr++ = rowStartIndex + i + 1;

      *indexPtr++ = nextRowStartIndex + i;
      *indexPtr++ = nextRowStartIndex + i + 1;
      *indexPtr++ = rowStartIndex + i + 1;
    }
  }

  return indexBytes;
}



//! Generates vertex buffer for DEM terrain tiles
class PlaneVertexBufferFunctor : public QBufferDataGenerator
{
  public:
    explicit PlaneVertexBufferFunctor( int resolution, float skirtHeight, const QByteArray &heightMap )
      : mResolution( resolution )
      , mSkirtHeight( skirtHeight )
      , mHeightMap( heightMap )
    {}

    QByteArray operator()() final
    {
      return createPlaneVertexData( mResolution, mSkirtHeight, mHeightMap );
    }

    bool operator ==( const QBufferDataGenerator &other ) const final
    {
      const PlaneVertexBufferFunctor *otherFunctor = functor_cast<PlaneVertexBufferFunctor>( &other );
      if ( otherFunctor != nullptr )
        return ( otherFunctor->mResolution == mResolution &&
                 otherFunctor->mSkirtHeight == mSkirtHeight &&
                 otherFunctor->mHeightMap == mHeightMap );
      return false;
    }

    QT3D_FUNCTOR( PlaneVertexBufferFunctor )

  private:
    int mResolution;
    float mSkirtHeight;
    QByteArray mHeightMap;
};


//! Generates index buffer for DEM terrain tiles
class PlaneIndexBufferFunctor : public QBufferDataGenerator
{
  public:
    explicit PlaneIndexBufferFunctor( int resolution, const QByteArray &heightMap )
      : mResolution( resolution )
      , mHeightMap( heightMap )
    {}

    QByteArray operator()() final
    {
      return createPlaneIndexData( mResolution, mHeightMap );
    }

    bool operator ==( const QBufferDataGenerator &other ) const final
    {
      const PlaneIndexBufferFunctor *otherFunctor = functor_cast<PlaneIndexBufferFunctor>( &other );
      if ( otherFunctor != nullptr )
        return ( otherFunctor->mResolution == mResolution );
      return false;
    }

    QT3D_FUNCTOR( PlaneIndexBufferFunctor )

  private:
    int mResolution;
    QByteArray mHeightMap;
};


// ------------


DemTerrainTileGeometry::DemTerrainTileGeometry( int resolution, float skirtHeight, const QByteArray &heightMap, DemTerrainTileGeometry::QNode *parent )
  : QGeometry( parent )
  , mResolution( resolution )
  , mSkirtHeight( skirtHeight )
  , mHeightMap( heightMap )
{
  init();
}

static bool intersectionDemTriangles( const QByteArray &vertexBuf, const QByteArray &indexBuf, const QgsRayCastingUtils::Ray3D &r, const QMatrix4x4 &worldTransform, QVector3D &intPt )
{
  // WARNING! this code is specific to how vertex buffers are built for DEM tiles,
  // it is not usable for any mesh...

  const float *vertices = reinterpret_cast<const float *>( vertexBuf.constData() );
  const uint *indices = reinterpret_cast<const uint *>( indexBuf.constData() );
  int vertexCnt = vertexBuf.count() / sizeof( float );
  int indexCnt = indexBuf.count() / sizeof( uint );
  Q_ASSERT( vertexCnt % 8 == 0 );
  Q_ASSERT( indexCnt % 3 == 0 );
  //int vertexCount = vertexCnt/8;
  int triangleCount = indexCnt / 3;

  QVector3D intersectionPt, minIntersectionPt;
  float distance;
  float minDistance = -1;

  for ( int i = 0; i < triangleCount; ++i )
  {
    int v0 = indices[i * 3], v1 = indices[i * 3 + 1], v2 = indices[i * 3 + 2];
    QVector3D a( vertices[v0 * 8], vertices[v0 * 8 + 1], vertices[v0 * 8 + 2] );
    QVector3D b( vertices[v1 * 8], vertices[v1 * 8 + 1], vertices[v1 * 8 + 2] );
    QVector3D c( vertices[v2 * 8], vertices[v2 * 8 + 1], vertices[v2 * 8 + 2] );

    const QVector3D tA = worldTransform * a;
    const QVector3D tB = worldTransform * b;
    const QVector3D tC = worldTransform * c;

    QVector3D uvw;
    float t = 0;
    if ( QgsRayCastingUtils::rayTriangleIntersection( r, tA, tB, tC, uvw, t ) )
    {
      intersectionPt = r.point( t * r.distance() );
      distance = r.projectedDistance( intersectionPt );

      // we only want the first intersection of the ray with the mesh (closest to the ray origin)
      if ( minDistance == -1 || distance < minDistance )
      {
        minDistance = distance;
        minIntersectionPt = intersectionPt;
      }
    }
  }

  if ( minDistance != -1 )
  {
    intPt = minIntersectionPt;
    return true;
  }
  else
    return false;
}

bool DemTerrainTileGeometry::rayIntersection( const QgsRayCastingUtils::Ray3D &ray, const QMatrix4x4 &worldTransform, QVector3D &intersectionPoint )
{
  return intersectionDemTriangles( mVertexBuffer->data(), mIndexBuffer->data(), ray, worldTransform, intersectionPoint );
}

void DemTerrainTileGeometry::init()
{
  mPositionAttribute = new QAttribute( this );
  mNormalAttribute = new QAttribute( this );
  mTexCoordAttribute = new QAttribute( this );
  mIndexAttribute = new QAttribute( this );
  mVertexBuffer = new Qt3DRender::QBuffer( Qt3DRender::QBuffer::VertexBuffer, this );
  mIndexBuffer = new Qt3DRender::QBuffer( Qt3DRender::QBuffer::IndexBuffer, this );

  int nVertsX = mResolution + 2;
  int nVertsZ = mResolution + 2;
  const int nVerts = nVertsX * nVertsZ;
  const int stride = ( 3 + 2 + 3 ) * sizeof( float );
  const int faces = 2 * ( nVertsX - 1 ) * ( nVertsZ - 1 );

  mPositionAttribute->setName( QAttribute::defaultPositionAttributeName() );
  mPositionAttribute->setVertexBaseType( QAttribute::Float );
  mPositionAttribute->setVertexSize( 3 );
  mPositionAttribute->setAttributeType( QAttribute::VertexAttribute );
  mPositionAttribute->setBuffer( mVertexBuffer );
  mPositionAttribute->setByteStride( stride );
  mPositionAttribute->setCount( nVerts );

  mTexCoordAttribute->setName( QAttribute::defaultTextureCoordinateAttributeName() );
  mTexCoordAttribute->setVertexBaseType( QAttribute::Float );
  mTexCoordAttribute->setVertexSize( 2 );
  mTexCoordAttribute->setAttributeType( QAttribute::VertexAttribute );
  mTexCoordAttribute->setBuffer( mVertexBuffer );
  mTexCoordAttribute->setByteStride( stride );
  mTexCoordAttribute->setByteOffset( 3 * sizeof( float ) );
  mTexCoordAttribute->setCount( nVerts );

  mNormalAttribute->setName( QAttribute::defaultNormalAttributeName() );
  mNormalAttribute->setVertexBaseType( QAttribute::Float );
  mNormalAttribute->setVertexSize( 3 );
  mNormalAttribute->setAttributeType( QAttribute::VertexAttribute );
  mNormalAttribute->setBuffer( mVertexBuffer );
  mNormalAttribute->setByteStride( stride );
  mNormalAttribute->setByteOffset( 5 * sizeof( float ) );
  mNormalAttribute->setCount( nVerts );

  mIndexAttribute->setAttributeType( QAttribute::IndexAttribute );
  mIndexAttribute->setVertexBaseType( QAttribute::UnsignedInt );
  mIndexAttribute->setBuffer( mIndexBuffer );

  // Each primitive has 3 vertives
  mIndexAttribute->setCount( faces * 3 );

  // switched to setting data instead of just setting data generators because we also need the buffers
  // available for ray-mesh intersections and we can't access the private copy of data in Qt (if there is any)
  mVertexBuffer->setData( PlaneVertexBufferFunctor( mResolution, mSkirtHeight, mHeightMap )() );
  mIndexBuffer->setData( PlaneIndexBufferFunctor( mResolution, mHeightMap )() );

  addAttribute( mPositionAttribute );
  addAttribute( mTexCoordAttribute );
  addAttribute( mNormalAttribute );
  addAttribute( mIndexAttribute );
}

/// @endcond
