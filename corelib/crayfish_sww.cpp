/*
Crayfish - A collection of tools for TUFLOW and other hydraulic modelling packages
Copyright (C) 2015 Lutra Consulting

info at lutraconsulting dot co dot uk
Lutra Consulting
23 Chestnut Close
Burgess Hill
West Sussex
RH15 8HN

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <netcdf.h>

#include "crayfish.h"
#include "crayfish_mesh.h"
#include "crayfish_dataset.h"
#include "crayfish_output.h"

// threshold for determining whether an element is active (wet)
// the format does not explicitly store that information so we
// determine that when loading data
#define DEPTH_THRESHOLD   0.0001   // in meters


Mesh* loadSWW(const QString& fileName, LoadStatus* status)
{
  if (status) status->clear();

  int ncid;
  int res;

  res = nc_open(fileName.toUtf8().constData(), NC_NOWRITE, &ncid);
  if (res != NC_NOERR)
  {
    qDebug("error: %s", nc_strerror(res));
    nc_close(ncid);
    if (status) status->mLastError = LoadStatus::Err_UnknownFormat;
    return 0;
  }

  // get dimensions
  int nVolumesId, nVerticesId, nPointsId, nTimestepsId;
  size_t nVolumes, nVertices, nPoints, nTimesteps;
  if (nc_inq_dimid(ncid, "number_of_volumes", &nVolumesId) != NC_NOERR ||
      nc_inq_dimid(ncid, "number_of_vertices", &nVerticesId) != NC_NOERR ||
      nc_inq_dimid(ncid, "number_of_points", &nPointsId) != NC_NOERR ||
      nc_inq_dimid(ncid, "number_of_timesteps", &nTimestepsId) != NC_NOERR)
  {
    nc_close(ncid);
    if (status) status->mLastError = LoadStatus::Err_UnknownFormat;
    return 0;
  }
  if (nc_inq_dimlen(ncid, nVolumesId, &nVolumes) != NC_NOERR ||
      nc_inq_dimlen(ncid, nVerticesId, &nVertices) != NC_NOERR ||
      nc_inq_dimlen(ncid, nPointsId, &nPoints) != NC_NOERR ||
      nc_inq_dimlen(ncid, nTimestepsId, &nTimesteps) != NC_NOERR)
  {
    nc_close(ncid);
    if (status) status->mLastError = LoadStatus::Err_UnknownFormat;
    return 0;
  }

  if (nVertices != 3)
  {
    qDebug("Expecting triangular elements!");
    nc_close(ncid);
    if (status) status->mLastError = LoadStatus::Err_UnknownFormat;
    return 0;
  }

  int xid, yid, zid, volumesid, timeid, stageid;
  if (nc_inq_varid(ncid, "x", &xid) != NC_NOERR ||
      nc_inq_varid(ncid, "y", &yid) != NC_NOERR ||
      nc_inq_varid(ncid, "z", &zid) != NC_NOERR ||
      nc_inq_varid(ncid, "volumes", &volumesid) != NC_NOERR ||
      nc_inq_varid(ncid, "time", &timeid) != NC_NOERR ||
      nc_inq_varid(ncid, "stage", &stageid) != NC_NOERR)
  {
    nc_close(ncid);
    if (status) status->mLastError = LoadStatus::Err_UnknownFormat;
    return 0;
  }

  // load mesh data
  QVector<float> px(nPoints), py(nPoints), pz(nPoints);
  unsigned int* pvolumes = new unsigned int[nVertices * nVolumes];
  if (nc_get_var_float (ncid, xid, px.data()) != NC_NOERR ||
      nc_get_var_float (ncid, yid, py.data()) != NC_NOERR ||
      nc_get_var_float (ncid, zid, pz.data()) != NC_NOERR ||
      nc_get_var_int (ncid, volumesid, (int *) pvolumes) != NC_NOERR)
  {
    delete [] pvolumes;

    nc_close(ncid);
    if (status) status->mLastError = LoadStatus::Err_UnknownFormat;
    return 0;
  }

  // we may need to apply a shift to the X,Y coordinates
  float xLLcorner = 0, yLLcorner = 0;
  nc_get_att_float(ncid, NC_GLOBAL, "xllcorner", &xLLcorner);
  nc_get_att_float(ncid, NC_GLOBAL, "yllcorner", &yLLcorner);

  // create output for bed elevation
  Output* o = new Output;
  o->init(nPoints, nVolumes, false);
  o->time = 0.0;
  memset(o->active.data(), 1, nVolumes); // All cells active


  Mesh::Nodes nodes(nPoints);
  Node* nodesPtr = nodes.data();
  for (int i = 0; i < nPoints; ++i, ++nodesPtr)
  {
    nodesPtr->id = i;
    nodesPtr->x = px[i] + xLLcorner;
    nodesPtr->y = py[i] + yLLcorner;
    o->values[i] = pz[i];
  }

  Mesh::Elements elements(nVolumes);
  Element* elementsPtr = elements.data();

  for (int i = 0; i < nVolumes; ++i, ++elementsPtr)
  {
    elementsPtr->id = i;
    elementsPtr->eType = Element::E3T;
    elementsPtr->p[0] = pvolumes[3*i+0];
    elementsPtr->p[1] = pvolumes[3*i+1];
    elementsPtr->p[2] = pvolumes[3*i+2];
  }

  delete [] pvolumes;

  Mesh* mesh = new Mesh(nodes, elements);

  // Create a dataset for the bed elevation
  DataSet* bedDs = new DataSet(fileName);
  bedDs->setType(DataSet::Bed);
  bedDs->setName("Bed Elevation");
  bedDs->setIsTimeVarying(false);
  bedDs->addOutput(o);  // takes ownership of the Output
  bedDs->updateZRange(nPoints);
  mesh->addDataSet(bedDs);

  // load results

  DataSet* ds = new DataSet(fileName);
  ds->setType(DataSet::Scalar);
  ds->setName("Depth");
  ds->setIsTimeVarying(true);

  QVector<float> times(nTimesteps);
  nc_get_var_float(ncid, timeid, times.data());

  const float* elev = o->values.constData();
  for (int t = 0; t < nTimesteps; ++t)
  {
    Output* to = new Output;
    to->init(nPoints, nVolumes, false);
    to->time = times[t] / 3600.;
    float* values = to->values.data();

    // fetching "stage" data for one timestep
    size_t start[2], count[2];
    const ptrdiff_t stride[2] = {1,1};
    start[0] = t;
    start[1] = 0;
    count[0] = 1;
    count[1] = nPoints;
    nc_get_vars_float(ncid, stageid, start, count, stride, values);

    for (int j = 0; j < nPoints; ++j)
      values[j] -= elev[j];

    // determine which elements are active (wet)
    for (int elemidx = 0; elemidx < nVolumes; ++elemidx)
    {
      const Element& elem = mesh->elements()[elemidx];
      float v0 = values[elem.p[0]];
      float v1 = values[elem.p[1]];
      float v2 = values[elem.p[2]];
      to->active[elemidx] = v0 > DEPTH_THRESHOLD || v1 > DEPTH_THRESHOLD || v2 > DEPTH_THRESHOLD;
    }

    ds->addOutput(to);
  }

  ds->updateZRange(nPoints);
  mesh->addDataSet(ds);

  int momentumxid, momentumyid;
  if (nc_inq_varid(ncid, "xmomentum", &momentumxid) == NC_NOERR &&
      nc_inq_varid(ncid, "ymomentum", &momentumyid) == NC_NOERR)
  {
    DataSet* mds = new DataSet(fileName);
    mds->setType(DataSet::Vector);
    mds->setName("Momentum");
    mds->setIsTimeVarying(true);

    QVector<float> valuesX(nPoints), valuesY(nPoints);
    for (int t = 0; t < nTimesteps; ++t)
    {
      Output* mto = new Output;
      mto->init(nPoints, nVolumes, true);
      mto->time = times[t] / 3600.;
      mto->active = ds->output(t)->active;

      // fetching "stage" data for one timestep
      size_t start[2], count[2];
      const ptrdiff_t stride[2] = {1,1};
      start[0] = t;
      start[1] = 0;
      count[0] = 1;
      count[1] = nPoints;
      nc_get_vars_float(ncid, momentumxid, start, count, stride, valuesX.data());
      nc_get_vars_float(ncid, momentumyid, start, count, stride, valuesY.data());

      Output::float2D* mtoValuesV = mto->valuesV.data();
      float* mtoValues = mto->values.data();
      for (int i = 0; i < nPoints; ++i)
      {
        mtoValuesV[i].x = valuesX[i];
        mtoValuesV[i].y = valuesY[i];
        mtoValues[i] = mtoValuesV[i].length();
      }

      mds->addOutput(mto);
    }


    mds->updateZRange(nPoints);
    mesh->addDataSet(mds);
  }

  nc_close(ncid);

  return mesh;
}
