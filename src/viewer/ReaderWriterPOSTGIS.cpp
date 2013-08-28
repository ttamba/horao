#include "PostGisUtils.h"

#include <osgDB/FileNameUtils>
#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osg/ShapeDrawable>
#include <osgUtil/Optimizer>

#include <iostream>
#include <sstream>
#include <algorithm>
#include <map>


#define DEBUG_OUT if (0) std::cout

// for RAII of connection
struct PostgisConnection
{
    PostgisConnection( const std::string & connInfo )
        : _conn( PQconnectdb(connInfo.c_str()) )
    {}

    operator bool(){ return CONNECTION_OK == PQstatus( _conn );  }

    ~PostgisConnection()
    {
        if (_conn) PQfinish( _conn );
    }

    // for RAII ok query results
    struct QueryResult
    {
        QueryResult( PostgisConnection & conn, const std::string & query )
            : _res( PQexec( conn._conn, query.c_str() ) )
            , _error( PQresultErrorMessage(_res) )
        {}

        ~QueryResult() 
        { 
            PQclear(_res);
        }

        operator bool() const { return _error.empty(); }

        PGresult * get(){ return _res; }

        const std::string & error() const { return _error; }

    private:
        PGresult * _res;
        const std::string _error;
        // non copyable
        QueryResult( const QueryResult & );
        QueryResult operator=( const QueryResult &); 
    };

private:
   PGconn * _conn;
};


struct ReaderWriterPOSTGIS : osgDB::ReaderWriter
{
    ReaderWriterPOSTGIS()
    {
        supportsExtension( "postgis", "PostGIS feature driver for osgEarth" );
        supportsExtension( "postgisd", "PostGIS feature driver for osgEarth" );
    }

    virtual const char* className()
    {
        return "PostGIS Feature Reader";
    }

    // note: stupid key="value" parser, value must not contain '"'  
    virtual ReadResult readNode(const std::string& file_name, const Options* ) const
    {
        if ( !acceptsExtension(osgDB::getLowerCaseFileExtension( file_name )))
            return ReadResult::FILE_NOT_HANDLED;

        std::cout << "loaded plugin postgis for [" << file_name << "]\n";

        osg::Timer timer;

        DEBUG_OUT << "connecting to postgis...\n";
        timer.setStartTick();

        typedef std::map< std::string, std::string > AttributeMap;
        AttributeMap am;
        std::stringstream line(file_name);
        std::string key, value;
        while (    std::getline( line, key, '=' ) 
                && std::getline( line, value, '"' ) 
                && std::getline( line, value, '"' )){
            // remove spaces in key
            key.erase( remove_if(key.begin(), key.end(), isspace ), key.end());
            DEBUG_OUT << "key=\"" << key << "\" value=\"" << value << "\"\n";
            am.insert( std::make_pair( key, value ) );
        }

        PostgisConnection conn( am["conn_info"] );
        if (!conn){
            std::cerr << "failed to open database with conn_info=\"" << am["conn_info"] << "\"\n";
            return ReadResult::FILE_NOT_FOUND;
        }

        DEBUG_OUT << "connected in " <<  timer.time_s() << "sec\n";

        DEBUG_OUT << "execute request...\n";
        timer.setStartTick();

        PostgisConnection::QueryResult res( conn, am["query"].c_str() );
        if (!res){
            std::cerr << "failed to execute query=\"" <<  am["query"] << "\" : " << res.error() << "\n";
            return ReadResult::ERROR_IN_READING_FILE;
        }

        const int featureIdIdx = PQfnumber(res.get(), am["feature_id"].c_str() );
        if ( featureIdIdx < 0 )
        {
            std::cerr << "failed to obtain feature_id=\""<< am["feature_id"] <<"\"\n";
            return ReadResult::ERROR_IN_READING_FILE;
        }

        const int geomIdx = PQfnumber(res.get(),  am["geometry_column"].c_str() );
        if ( geomIdx < 0 )
        {
            std::cerr << "failed to obtain geometry_column=\""<< am["geometry_column"] <<"\"\n";
            return ReadResult::ERROR_IN_READING_FILE;
        }

        const int numFeatures = PQntuples( res.get() );
        DEBUG_OUT << "got " << numFeatures << " features in " << timer.time_s() << "sec\n";

        DEBUG_OUT << "converting " << numFeatures << " features from postgis...\n";
        timer.setStartTick();

        // define transfo  layerToWord
        osg::Matrixd layerToWord;
        {
            osg::Vec3d center(0,0,0);
            if ( !( std::stringstream( am["center"] ) >> center.x() >> center.y() ) ){
                std::cerr << "failed to obtain center=\""<< am["center"] <<"\"\n";
                return ReadResult::ERROR_IN_READING_FILE;
            }
            layerToWord.makeTranslate( -center );
        }

        osg::ref_ptr<osg::Geode> group = new osg::Geode();

        Stack3d::Viewer::TriangleMesh mesh( layerToWord );

        for( int i=0; i<numFeatures; i++ )
        {
            const char * wkb = PQgetvalue( res.get(), i, geomIdx );
            Stack3d::Viewer::Lwgeom lwgeom( wkb, Stack3d::Viewer::Lwgeom::WKB() );
            assert( lwgeom.get() );
            mesh.push_back( lwgeom.get() );
        }
        group->addDrawable( mesh.createGeometry() );

        DEBUG_OUT << "converted " << numFeatures << " features in " << timer.time_s() << "sec\n";

        return group.release();
    }
};

REGISTER_OSGPLUGIN(postgis, ReaderWriterPOSTGIS)

