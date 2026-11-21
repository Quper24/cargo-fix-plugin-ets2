#pragma once
#include <algorithm>
#include <string>
#include <vector>
namespace coaches {
// Source and final destination count towards the user-selected 24-point limit.
inline bool ValidRoute(const std::vector<std::string>& points){
    if(points.size()<2||points.size()>24)return false;
    for(std::size_t i=0;i<points.size();++i){
        if(points[i].empty()||std::find(points.begin(),points.begin()+i,points[i])!=points.begin()+i)return false;
    }
    return true;
}
}
